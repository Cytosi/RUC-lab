/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <cstring>
#include <vector>

#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

namespace {

char *build_index_key(const IndexMeta &index, const RmRecord &record) {
    char *key = new char[index.col_tot_len];
    int offset = 0;
    for (size_t i = 0; i < index.col_num; ++i) {
        memcpy(key + offset, record.data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
    return key;
}

void rollback_insert(Transaction *txn, SmManager *sm_manager, WriteRecord *write_record) {
    auto &tab_name = write_record->GetTableName();
    auto fh = sm_manager->fhs_.at(tab_name).get();
    auto &tab = sm_manager->db_.get_table(tab_name);
    auto rid = write_record->GetRid();

    if (!fh->is_record(rid)) {
        return;
    }

    auto record = fh->get_record(rid, nullptr);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        char *key = build_index_key(index, *record);
        ih->delete_entry(key, txn);
        delete[] key;
    }
    fh->delete_record(rid, nullptr);
}

void rollback_delete(Transaction *txn, SmManager *sm_manager, WriteRecord *write_record) {
    auto &tab_name = write_record->GetTableName();
    auto fh = sm_manager->fhs_.at(tab_name).get();
    auto &tab = sm_manager->db_.get_table(tab_name);
    auto &record = write_record->GetRecord();
    auto rid = write_record->GetRid();

    if (!fh->is_record(rid)) {
        fh->insert_record(rid, record.data);
    }
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        char *key = build_index_key(index, record);
        ih->insert_entry(key, rid, txn);
        delete[] key;
    }
}

void rollback_update(Transaction *txn, SmManager *sm_manager, WriteRecord *write_record) {
    auto &tab_name = write_record->GetTableName();
    auto fh = sm_manager->fhs_.at(tab_name).get();
    auto &tab = sm_manager->db_.get_table(tab_name);
    auto &old_record = write_record->GetRecord();
    auto rid = write_record->GetRid();

    auto current_record = fh->get_record(rid, nullptr);
    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        char *new_key = build_index_key(index, *current_record);
        ih->delete_entry(new_key, txn);
        delete[] new_key;
    }

    fh->update_record(rid, old_record.data, nullptr);

    for (auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        char *old_key = build_index_key(index, old_record);
        ih->insert_entry(old_key, rid, txn);
        delete[] old_key;
    }
}

}  // namespace

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        txn = new Transaction(next_txn_id_++);
    }

    txn->set_start_ts(next_timestamp_++);
    txn->set_state(TransactionState::GROWING);

    if (log_manager != nullptr) {
        BeginLogRecord begin_log(txn->get_transaction_id());
        begin_log.prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(&begin_log));
    }

    std::unique_lock<std::mutex> lock(latch_);
    txn_map[txn->get_transaction_id()] = txn;
    return txn;
}

void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr) {
        return;
    }

    if (log_manager != nullptr) {
        CommitLogRecord commit_log(txn->get_transaction_id());
        commit_log.prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(&commit_log));
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::COMMITTED);

    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (auto &lock_data_id : locks) {
        lock_manager_->unlock(txn, lock_data_id);
    }

    auto write_set = txn->get_write_set();
    for (auto *write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    lock_set->clear();
}

void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }

    auto write_set = txn->get_write_set();
    for (auto it = write_set->rbegin(); it != write_set->rend(); ++it) {
        auto *write_record = *it;
        switch (write_record->GetWriteType()) {
            case WType::INSERT_TUPLE:
                rollback_insert(txn, sm_manager_, write_record);
                break;
            case WType::DELETE_TUPLE:
                rollback_delete(txn, sm_manager_, write_record);
                break;
            case WType::UPDATE_TUPLE:
                rollback_update(txn, sm_manager_, write_record);
                break;
        }
    }

    if (log_manager != nullptr) {
        AbortLogRecord abort_log(txn->get_transaction_id());
        abort_log.prev_lsn_ = txn->get_prev_lsn();
        txn->set_prev_lsn(log_manager->add_log_to_buffer(&abort_log));
        log_manager->flush_log_to_disk();
    }

    txn->set_state(TransactionState::ABORTED);

    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (auto &lock_data_id : locks) {
        lock_manager_->unlock(txn, lock_data_id);
    }

    for (auto *write_record : *write_set) {
        delete write_record;
    }
    write_set->clear();
    lock_set->clear();
}
