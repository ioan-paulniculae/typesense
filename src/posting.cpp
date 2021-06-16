#include "posting.h"
#include "posting_list.h"

int64_t compact_posting_list_t::upsert(const uint32_t id, const std::vector<uint32_t>& offsets) {
    return upsert(id, &offsets[0], offsets.size());
}

int64_t compact_posting_list_t::upsert(const uint32_t id, const uint32_t* offsets, uint32_t num_offsets) {
    // format: num_offsets, offset1,..,offsetn, id1 | num_offsets, offset1,..,offsetn, id2
    uint32_t last_id = (length == 0) ? 0 : id_offsets[length - 1];
    int64_t extra_length_needed = 0;

    if(length == 0 || id > last_id) {
        extra_length_needed = (num_offsets + 2);
        if(length + extra_length_needed > capacity) {
            // enough storage should have been provided upstream
            return (length + extra_length_needed) - capacity;
        }

        // can just append to the end
        id_offsets[length++] = num_offsets;
        for(size_t i = 0; i < num_offsets; i++) {
            id_offsets[length+i] = offsets[i];
        }
        length += num_offsets;
        id_offsets[length++] = id;
        ids_length++;
    } else {
        // locate position and shift contents to make space available
        int64_t i = 0;

        while(i < length) {
            size_t num_existing_offsets = id_offsets[i];
            size_t existing_id = id_offsets[i + num_existing_offsets + 1];

            if(existing_id == id) {
                extra_length_needed = (num_offsets - num_existing_offsets);
                if(extra_length_needed > 0) {
                    if(length + extra_length_needed > capacity) {
                        // enough storage should have been provided upstream
                        return (length + extra_length_needed) - capacity;
                    }

                    // shift offsets to the right to make space
                    int64_t shift_index = int64_t(length) + extra_length_needed - 1;
                    while(shift_index >= i && (shift_index - extra_length_needed) >= 0) {
                        id_offsets[shift_index] = id_offsets[shift_index - extra_length_needed];
                        shift_index--;
                    }

                } else if(extra_length_needed < 0) {
                    // shift offsets to the left to reduce space
                    // [num_offsets][0][2][4][id]
                    // [num_offsets][0][id]
                    size_t offset_diff = (num_existing_offsets - num_offsets);
                    size_t start_index = i + 1 + offset_diff;
                    while(start_index < length - offset_diff) {
                       id_offsets[start_index] = id_offsets[start_index + offset_diff];
                       start_index++;
                    }
                }

                id_offsets[i] = num_offsets;
                for(size_t j = 0; j < num_offsets; j++) {
                    id_offsets[i + 1 + j] = offsets[j];
                }

                id_offsets[i+1+num_offsets] = id;

                break;
            }

            else if(existing_id > id) {
                extra_length_needed = (num_offsets + 2);
                if(length + extra_length_needed > capacity) {
                    // enough storage should have been provided upstream
                    return (length + extra_length_needed) - capacity;
                }

                // shift index [i..length-1] by `extra_length_needed` positions
                int64_t shift_index = length + extra_length_needed - 1;
                while((shift_index - extra_length_needed) >= 0 && shift_index >= i) {
                    // [*1 1 4]        [1 1 7]
                    //        [1 1 3]
                    id_offsets[shift_index] = id_offsets[shift_index - extra_length_needed];
                    shift_index--;
                }
                // now store the new offsets in the shifted space
                id_offsets[i++] = num_offsets;
                for (size_t j = 0; j < num_offsets; j++) {
                    id_offsets[i+j] = offsets[j];
                }

                i += num_offsets;
                id_offsets[i++] = id;
                ids_length++;
                break;
            }

            i += num_existing_offsets + 2;
        }

        length += extra_length_needed;   // extra_length_needed can be negative here but that's okay
    }

    return 0;
}

void compact_posting_list_t::erase(const uint32_t id) {
    // locate position and shift contents to collapse space vacated
    size_t i = 0;
    while(i < length) {
        size_t num_existing_offsets = id_offsets[i];
        size_t existing_id = id_offsets[i + num_existing_offsets + 1];
        if(existing_id > id) {
            // not found!
            return ;
        }

        if(existing_id == id) {
            size_t shift_offset = num_existing_offsets + 2;
            while(i+shift_offset < length) {
                id_offsets[i] = id_offsets[i+shift_offset];
                i++;
            }

            length -= shift_offset;
            break;
        }

        i += num_existing_offsets + 2;
    }

    ids_length--;
}

compact_posting_list_t* compact_posting_list_t::create(uint32_t num_ids, const uint32_t* ids, const uint32_t* offset_index,
                                                       uint32_t num_offsets, uint32_t* offsets) {
    // format: num_offsets, offset1,..,offsetn, id1 | num_offsets, offset1,..,offsetn, id2

    size_t length_required = num_offsets + (2 * num_ids);
    compact_posting_list_t* pl = (compact_posting_list_t*) malloc(sizeof(compact_posting_list_t) +
                                                                  (length_required * sizeof(uint32_t)));

    pl->length = 0;
    pl->capacity = length_required;
    pl->ids_length = 0;

    for(size_t i = 0; i < num_ids; i++) {
        uint32_t start_offset = offset_index[i];
        uint32_t next_start_offset = (i == num_ids-1) ? num_offsets : offset_index[i+1];
        pl->upsert(ids[i], offsets+start_offset, (next_start_offset - start_offset));
    }

    return pl;
}

posting_list_t* compact_posting_list_t::to_full_posting_list() {
    posting_list_t* pl = new posting_list_t(1024);

    size_t i = 0;
    while(i < length) {
        size_t num_existing_offsets = id_offsets[i];
        i++;
        std::vector<uint32_t> offsets(num_existing_offsets);
        for(size_t j = 0; j < num_existing_offsets; j++) {
            auto offset = id_offsets[i + j];
            offsets[j] = offset;
        }

        size_t existing_id = id_offsets[i + num_existing_offsets];
        pl->upsert(existing_id, offsets);
        i += num_existing_offsets + 1;
    }

    return pl;
}

uint32_t compact_posting_list_t::last_id() {
    return (length == 0) ? UINT32_MAX : id_offsets[length - 1];
}

uint32_t compact_posting_list_t::num_ids() const {
    return ids_length;
}

uint32_t compact_posting_list_t::first_id() {
    if(length == 0) {
        return 0;
    }

    return id_offsets[id_offsets[0] + 1];
}

bool compact_posting_list_t::contains(uint32_t id) {
    size_t i = 0;
    while(i < length) {
        size_t num_existing_offsets = id_offsets[i];
        size_t existing_id = id_offsets[i + num_existing_offsets + 1];

        if(existing_id > id) {
            // not found!
            return false;
        }

        if(existing_id == id) {
            return true;
        }

        i += num_existing_offsets + 2;
    }

    return false;
}

bool compact_posting_list_t::contains_atleast_one(const uint32_t* target_ids, size_t target_ids_size) {
    size_t i = 0;
    size_t target_ids_index = 0;

    while(i < length && target_ids_index < target_ids_size) {
        size_t num_existing_offsets = id_offsets[i];
        size_t existing_id = id_offsets[i + num_existing_offsets + 1];

        if(existing_id == target_ids[target_ids_index]) {
            return true;
        }

        if(target_ids[target_ids_index] < existing_id) {
            while(target_ids_index < target_ids_size && target_ids[target_ids_index] < existing_id) {
                target_ids_index++;
            }
        } else {
            i += num_existing_offsets + 2;
        }
    }

    return false;
}

/* posting operations */

void posting_t::upsert(void*& obj, uint32_t id, const std::vector<uint32_t>& offsets) {
    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = (compact_posting_list_t*) RAW_POSTING_PTR(obj);
        int64_t extra_capacity_required = list->upsert(id, offsets);

        if(extra_capacity_required != 0) {
            // grow the container by 30%
            size_t new_capacity = (list->capacity + extra_capacity_required) * 1.3;
            size_t new_capacity_bytes = sizeof(compact_posting_list_t) + (new_capacity * sizeof(uint32_t));
            auto new_list = (compact_posting_list_t *) realloc(list, new_capacity_bytes);
            if(new_list == nullptr) {
                abort();
            }

            list = new_list;
            list->capacity = new_capacity;
            obj = SET_COMPACT_POSTING(list);

            list->upsert(id, offsets);
        }

        if(list->length > COMPACT_LIST_THRESHOLD_LENGTH) {
            // we will store anything over this threshold as a full posting list
            posting_list_t* full_list = list->to_full_posting_list();
            free(list);
            obj = full_list;
            return;
        }

    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        list->upsert(id, offsets);
    }
}

void posting_t::erase(void*& obj, uint32_t id) {
    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = (compact_posting_list_t*) RAW_POSTING_PTR(obj);
        list->erase(id);

        // if the list becomes too small, we resize it to save memory
        if(list->length < list->capacity/2) {
            // resize container
            size_t new_capacity = list->capacity/2;
            size_t new_capacity_bytes = sizeof(compact_posting_list_t) + (new_capacity * sizeof(uint32_t));
            auto new_list = (compact_posting_list_t *) realloc(list, new_capacity_bytes);
            if(new_list == nullptr) {
                abort();
            }

            list = new_list;
            list->capacity = new_capacity;
            obj = SET_COMPACT_POSTING(list);
        }

    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        list->erase(id);
        if(list->num_blocks() == 1 && list->get_root()->size() <= 10) {
            // convert to compact posting format
            auto root_block = list->get_root();
            auto ids = root_block->ids.uncompress();
            auto offset_index = root_block->offset_index.uncompress();
            auto offsets = root_block->offsets.uncompress();

            compact_posting_list_t* compact_list = compact_posting_list_t::create(
                root_block->size(), ids, offset_index, root_block->offsets.getLength(), offsets
            );

            delete [] ids;
            delete [] offset_index;
            delete [] offsets;
            free(list);

            obj = COMPACT_POSTING_PTR(compact_list);
        }
    }
}

uint32_t posting_t::num_ids(const void* obj) {
    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = COMPACT_POSTING_PTR(obj);
        return list->num_ids();
    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        return list->num_ids();
    }
}

uint32_t posting_t::first_id(const void* obj) {
    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = COMPACT_POSTING_PTR(obj);
        return list->first_id();
    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        return list->first_id();
    }
}

bool posting_t::contains(const void* obj, uint32_t id) {
    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = COMPACT_POSTING_PTR(obj);
        return list->contains(id);
    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        return list->contains(id);
    }
}

bool posting_t::contains_atleast_one(const void* obj, const uint32_t* target_ids, size_t target_ids_size) {
    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = COMPACT_POSTING_PTR(obj);
        return list->contains_atleast_one(target_ids, target_ids_size);
    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        return list->contains_atleast_one(target_ids, target_ids_size);
    }
}

void posting_t::merge(const std::vector<void*>& raw_posting_lists, std::vector<uint32_t>& result_ids) {
    // we will have to convert the compact posting list (if any) to full form
    std::vector<posting_list_t*> plists;
    std::vector<uint32_t> expanded_plist_indices;
    to_expanded_plists(raw_posting_lists, plists, expanded_plist_indices);

    posting_list_t::merge(plists, result_ids);

    for(uint32_t expanded_plist_index: expanded_plist_indices) {
        delete plists[expanded_plist_index];
    }
}

void posting_t::intersect(const std::vector<void*>& raw_posting_lists, std::vector<uint32_t>& result_ids) {
    // we will have to convert the compact posting list (if any) to full form
    std::vector<posting_list_t*> plists;
    std::vector<uint32_t> expanded_plist_indices;
    to_expanded_plists(raw_posting_lists, plists, expanded_plist_indices);

    posting_list_t::intersect(plists, result_ids);

    for(uint32_t expanded_plist_index: expanded_plist_indices) {
        delete plists[expanded_plist_index];
    }
}

void posting_t::to_expanded_plists(const std::vector<void*>& raw_posting_lists, std::vector<posting_list_t*>& plists,
                                   std::vector<uint32_t>& expanded_plist_indices) {
    for(size_t i = 0; i < raw_posting_lists.size(); i++) {
        auto raw_posting_list = raw_posting_lists[i];

        if(IS_COMPACT_POSTING(raw_posting_list)) {
            auto compact_posting_list = COMPACT_POSTING_PTR(raw_posting_list);
            plists.emplace_back(compact_posting_list->to_full_posting_list());
            expanded_plist_indices.push_back(i);
        } else {
            posting_list_t* full_posting_list = (posting_list_t*) RAW_POSTING_PTR(raw_posting_list);
            plists.emplace_back(full_posting_list);
        }
    }
}

void posting_t::destroy_list(void*& obj) {
    if(obj == nullptr) {
        return;
    }

    if(IS_COMPACT_POSTING(obj)) {
        compact_posting_list_t* list = COMPACT_POSTING_PTR(obj);
        free(list); // assigned via malloc, so must be free()d
    } else {
        posting_list_t* list = (posting_list_t*) RAW_POSTING_PTR(obj);
        delete list;
    }

    obj = nullptr;
}
