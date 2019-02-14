// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/tablet_meta.h"

#include "olap/file_helper.h"
#include "olap/olap_common.h"
#include "olap/olap_define.h"
#include "olap/tablet_meta_manager.h"

namespace doris {

AlterTabletTask::AlterTabletTask()
    : _alter_state(AlterTabletState::ALTER_NONE),
      _related_tablet_id(-1),
      _related_schema_hash(-1) {}

OLAPStatus AlterTabletTask::init_from_pb(const AlterTabletPB& alter_task) {
    _alter_state = alter_task.alter_state();
    _related_tablet_id = alter_task.related_tablet_id();
    _related_schema_hash = alter_task.related_schema_hash();
    _alter_type = alter_task.alter_type();
    for (auto& rs_meta_pb : alter_task.rowsets_to_alter()) {
        RowsetMetaSharedPtr rs_meta(new RowsetMeta());
        rs_meta->init_from_pb(rs_meta_pb);
        _rowsets_to_alter.push_back(rs_meta);
    }
    return OLAP_SUCCESS;
}

OLAPStatus AlterTabletTask::to_alter_pb(AlterTabletPB* alter_task) {
    alter_task->set_alter_state(_alter_state);
    alter_task->set_related_tablet_id(_related_tablet_id);
    alter_task->set_related_schema_hash(_related_schema_hash);
    alter_task->set_alter_type(_alter_type);
    for (auto& rs : _rowsets_to_alter) {
        rs->to_rowset_pb(alter_task->add_rowsets_to_alter());
    }
    return OLAP_SUCCESS;
}

OLAPStatus AlterTabletTask::clear() {
    _alter_state = AlterTabletState::ALTER_NONE;
    _related_tablet_id = -1;
    _related_schema_hash = -1;
    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::create(int64_t table_id, int64_t partition_id,
                              int64_t tablet_id, int64_t schema_hash,
                              uint64_t shard_id, const TTabletSchema& tablet_schema,
                              uint32_t next_unique_id,
                              const std::unordered_map<uint32_t, uint32_t>& col_ordinal_to_unique_id,
                              TabletMeta** tablet_meta) {
    *tablet_meta = new TabletMeta(table_id, partition_id,
                                  tablet_id, schema_hash,
                                  shard_id, tablet_schema,
                                  next_unique_id, col_ordinal_to_unique_id);
    return OLAP_SUCCESS;
}

TabletMeta::TabletMeta() {}

TabletMeta::TabletMeta(DataDir* data_dir) {
    _data_dir = data_dir;
}

TabletMeta::TabletMeta(int64_t table_id, int64_t partition_id,
                       int64_t tablet_id, int64_t schema_hash,
                       uint64_t shard_id, const TTabletSchema& tablet_schema,
                       uint32_t next_unique_id,
                       const std::unordered_map<uint32_t, uint32_t>& col_ordinal_to_unique_id) {
    _tablet_meta_pb.set_table_id(table_id);
    _tablet_meta_pb.set_partition_id(partition_id);
    _tablet_meta_pb.set_tablet_id(tablet_id);
    _tablet_meta_pb.set_schema_hash(schema_hash);
    _tablet_meta_pb.set_shard_id(shard_id);
    _tablet_meta_pb.set_creation_time(time(NULL));
    _tablet_meta_pb.set_cumulative_layer_point(-1);
    TabletSchemaPB* schema = _tablet_meta_pb.mutable_schema();
    schema->set_num_short_key_columns(tablet_schema.short_key_column_count);
    schema->set_num_rows_per_row_block(config::default_num_rows_per_column_file_block);
    switch(tablet_schema.keys_type) {
        case TKeysType::DUP_KEYS:
            schema->set_keys_type(KeysType::DUP_KEYS);
            break;
        case TKeysType::UNIQUE_KEYS:
            schema->set_keys_type(KeysType::UNIQUE_KEYS);
            break;
        case TKeysType::AGG_KEYS:
            schema->set_keys_type(KeysType::AGG_KEYS);
            break;
        default:
            LOG(WARNING) << "unknown tablet keys type";
            break;
    }
    schema->set_compress_kind(COMPRESS_LZ4);

    // set column information
    uint32_t col_ordinal = 0;
    uint32_t key_count = 0;
    bool has_bf_columns = false;
    for (TColumn tcolumn : tablet_schema.columns) {
        ColumnPB* column = schema->add_column();
        uint32_t unique_id = col_ordinal_to_unique_id.at(col_ordinal);
        column->set_unique_id(unique_id);
        column->set_name(tcolumn.column_name);
        string data_type;
        EnumToString(TPrimitiveType, tcolumn.column_type.type, data_type);
        column->set_type(data_type);
        if (tcolumn.column_type.type == TPrimitiveType::DECIMAL) {
            column->set_precision(tcolumn.column_type.precision);
            column->set_frac(tcolumn.column_type.scale);
        }
        uint32_t length = FieldInfo::get_field_length_by_type(
                tcolumn.column_type.type, tcolumn.column_type.len);
                column->set_length(length);
        column->set_index_length(length);
        if (tcolumn.column_type.type == TPrimitiveType::VARCHAR || tcolumn.column_type.type == TPrimitiveType::HLL) {
            if (!tcolumn.column_type.__isset.index_len) {
                column->set_index_length(10);
            } else {
                column->set_index_length(tcolumn.column_type.index_len);
            }
        }
        if (!tcolumn.is_key) {
            column->set_is_key(false);
            string aggregation_type;
            EnumToString(TAggregationType, tcolumn.aggregation_type, aggregation_type);
            column->set_aggregation(aggregation_type);
        } else {
            ++key_count;
            column->set_is_key(true);
            column->set_aggregation("NONE");
        }
        column->set_is_nullable(tcolumn.is_allow_null);
        if (tcolumn.__isset.default_value) {
            column->set_default_value(tcolumn.default_value);
        }
        if (tcolumn.__isset.is_bloom_filter_column) {
            column->set_is_bf_column(tcolumn.is_bloom_filter_column);
            has_bf_columns = true;
        }
    }

    schema->set_next_column_unique_id(next_unique_id);
    if (has_bf_columns && tablet_schema.__isset.bloom_filter_fpp) {
        schema->set_bf_fpp(tablet_schema.bloom_filter_fpp);
    }

    init_from_pb(_tablet_meta_pb);
}

OLAPStatus TabletMeta::save(const string& file_path) {
    DCHECK(!file_path.empty());

    FileHeader<TabletMetaPB> file_header;
    FileHandler file_handler;

    if (file_handler.open_with_mode(file_path.c_str(),
            O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR) != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to open header file. file='" << file_path;
        return OLAP_ERR_IO_ERROR;
    }

    try {
        file_header.mutable_message()->CopyFrom(_tablet_meta_pb);
    } catch (...) {
        LOG(WARNING) << "fail to copy protocol buffer object. file='" << file_path;
        return OLAP_ERR_OTHER_ERROR;
    }

    if (file_header.prepare(&file_handler) != OLAP_SUCCESS
            || file_header.serialize(&file_handler) != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to serialize to file header. file='" << file_path;
        return OLAP_ERR_SERIALIZE_PROTOBUF_ERROR;
    }

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::create_from_file(const std::string& file_path) {
    FileHeader<TabletMetaPB> file_header;
    FileHandler file_handler;

    if (file_handler.open(file_path.c_str(), O_RDONLY) != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to open ordinal file. file=" << file_path;
        return OLAP_ERR_IO_ERROR;
    }

    // In file_header.unserialize(), it validates file length, signature, checksum of protobuf.
    if (file_header.unserialize(&file_handler) != OLAP_SUCCESS) {
        LOG(WARNING) << "fail to unserialize tablet_meta. file='" << file_path;
        return OLAP_ERR_PARSE_PROTOBUF_ERROR;
    }

    try {
       _tablet_meta_pb.CopyFrom(file_header.message());
    } catch (...) {
        LOG(WARNING) << "fail to copy protocol buffer object. file='" << file_path;
        return OLAP_ERR_PARSE_PROTOBUF_ERROR;
    }

    
    return init_from_pb(_tablet_meta_pb);
}

OLAPStatus TabletMeta::serialize(string* meta_binary) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return serialize_unlock(meta_binary);
};

OLAPStatus TabletMeta::serialize_unlock(string* meta_binary) const {
    _tablet_meta_pb.SerializeToString(meta_binary);
    return OLAP_SUCCESS;
};

OLAPStatus TabletMeta::deserialize(const string& meta_binary) {
    std::lock_guard<std::mutex> lock(_mutex);
    _tablet_meta_pb.ParseFromString(meta_binary);
    return init_from_pb(_tablet_meta_pb);
}

OLAPStatus TabletMeta::init_from_pb(const TabletMetaPB& tablet_meta_pb) {
    _table_id = tablet_meta_pb.table_id();
    _partition_id = tablet_meta_pb.partition_id();
    _tablet_id = tablet_meta_pb.tablet_id();
    _schema_hash = tablet_meta_pb.schema_hash();
    _shard_id = tablet_meta_pb.shard_id();
    _creation_time = tablet_meta_pb.creation_time();
    _cumulative_layer_point = tablet_meta_pb.cumulative_layer_point();
    RETURN_NOT_OK(_schema.init_from_pb(tablet_meta_pb.schema()));
    for (auto& it : tablet_meta_pb.rs_metas()) {
        RowsetMetaSharedPtr rs_meta(new RowsetMeta());
        rs_meta->init_from_pb(it);
        _rs_metas.push_back(std::move(rs_meta));
    }
    for (auto& it : tablet_meta_pb.inc_rs_metas()) {
        RowsetMetaSharedPtr rs_meta(new RowsetMeta());
        rs_meta->init_from_pb(it);
        _inc_rs_metas.push_back(std::move(rs_meta));
    }

    // generate TabletState
    switch (tablet_meta_pb.tablet_state()) {
        case PB_NOTREADY:
            _tablet_state = TabletState::TABLET_NOTREADY;
            break;
        case PB_RUNNING:
            _tablet_state = TabletState::TABLET_RUNNING;
            break;
        case PB_TOMBSTONED:
            _tablet_state = TabletState::TABLET_TOMBSTONED;
            break;
        case PB_STOPPED:
            _tablet_state = TabletState::TABLET_STOPPED;
            break;
        case PB_SHUTDOWN:
            _tablet_state = TabletState::TABLET_SHUTDOWN;
            break;
        default:
            LOG(WARNING) << "tablet has no state. tablet=" << _tablet_id
                          << ", schema_hash=" << _schema_hash;
    }

    // generate AlterTabletTask
    if (tablet_meta_pb.has_alter_tablet_task()) {
        RETURN_NOT_OK(_alter_task.init_from_pb(tablet_meta_pb.alter_tablet_task()));
    }
    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::save_meta() {
    std::lock_guard<std::mutex> lock(_mutex);
    return save_meta_unlock();
}

OLAPStatus TabletMeta::save_meta_unlock() {
    string meta_binary;
    serialize_unlock(&meta_binary);
    OLAPStatus status = TabletMetaManager::save(_data_dir, _tablet_id, _schema_hash, meta_binary);
    if (status != OLAP_SUCCESS) {
       LOG(WARNING) << "fail to save tablet_meta. status=" << status
                    << ", tablet_id=" << _tablet_id
                    << ", schema_hash=" << _schema_hash;
    }
    return status;
}

OLAPStatus TabletMeta::to_tablet_pb(TabletMetaPB* tablet_meta_pb) {
    std::lock_guard<std::mutex> lock(_mutex);
    return to_tablet_pb_unlock(tablet_meta_pb);
}

OLAPStatus TabletMeta::to_tablet_pb_unlock(TabletMetaPB* tablet_meta_pb) {
    tablet_meta_pb->set_table_id(_table_id);
    tablet_meta_pb->set_partition_id(_partition_id);
    tablet_meta_pb->set_tablet_id(_tablet_id);
    tablet_meta_pb->set_schema_hash(_schema_hash);
    tablet_meta_pb->set_shard_id(_shard_id);
    tablet_meta_pb->set_creation_time(_creation_time);
    tablet_meta_pb->set_cumulative_layer_point(_cumulative_layer_point);

    for (auto& rs : _rs_metas) {
        rs->to_rowset_pb(tablet_meta_pb->add_rs_metas());
    }
    for (auto rs : _inc_rs_metas) {
        rs->to_rowset_pb(tablet_meta_pb->add_inc_rs_metas());
    }
    _schema.to_schema_pb(tablet_meta_pb->mutable_schema());

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::add_rs_meta(const RowsetMetaSharedPtr& rs_meta) {
    std::lock_guard<std::mutex> lock(_mutex);

    // check RowsetMeta is valid
    for (auto& rs : _rs_metas) {
        if (rs->start_version() == rs_meta->start_version()
            && rs->end_version() == rs_meta->end_version()) {
            LOG(WARNING) << "rowset already exist. rowset_id=" << rs->rowset_id();
            return OLAP_ERR_ROWSET_ALREADY_EXIST;
        }
    }

    _rs_metas.push_back(std::move(rs_meta));
    RowsetMetaPB* rs_meta_pb = _tablet_meta_pb.add_rs_metas();
    rs_meta->to_rowset_pb(rs_meta_pb);

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::modify_rs_metas(const vector<RowsetMetaSharedPtr>& to_add,
                                       const vector<RowsetMetaSharedPtr>& to_delete) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto del_rs : to_delete) {
        auto it = _rs_metas.begin();
        while (it != _rs_metas.end()) {
            if (del_rs->version().first == (*it)->version().first
                  && del_rs->version().second == (*it)->version().second) {
                _rs_metas.erase(it);
            }
        }
    }

    for (auto rs : to_add) {
        _rs_metas.push_back(std::move(rs));
    }

    TabletMetaPB tablet_meta_pb;
    RETURN_NOT_OK(to_tablet_pb_unlock(&tablet_meta_pb));
    _tablet_meta_pb = std::move(tablet_meta_pb);
    RETURN_NOT_OK(save_meta_unlock());

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::revise_rs_metas(const std::vector<RowsetMetaSharedPtr>& rs_metas) {
    std::lock_guard<std::mutex> lock(_mutex);
    // delete alter task
    _tablet_meta_pb.clear_alter_tablet_task();
    _alter_task.clear(); 

    // remove all old rs_meta and add new rs_meta
    _tablet_meta_pb.clear_rs_metas();
    _rs_metas.clear();

    for (auto& rs_meta : rs_metas) {
        _rs_metas.push_back(rs_meta);
    }

    TabletMetaPB tablet_meta_pb;
    RETURN_NOT_OK(to_tablet_pb_unlock(&tablet_meta_pb));
    _tablet_meta_pb = std::move(tablet_meta_pb);
    RETURN_NOT_OK(save_meta_unlock());

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::add_inc_rs_meta(const RowsetMetaSharedPtr& rs_meta) {
    std::lock_guard<std::mutex> lock(_mutex);

    // check RowsetMeta is valid
    for (auto rs : _inc_rs_metas) {
        if (rs->start_version() == rs_meta->start_version()
            && rs->end_version() == rs_meta->end_version()) {
            LOG(WARNING) << "rowset already exist. rowset_id=" << rs->rowset_id();
            return OLAP_ERR_ROWSET_ALREADY_EXIST;
        }
    }

    _inc_rs_metas.push_back(std::move(rs_meta));
    RowsetMetaPB* rs_meta_pb = _tablet_meta_pb.add_inc_rs_metas();
    rs_meta->to_rowset_pb(rs_meta_pb);
    RETURN_NOT_OK(save_meta_unlock());

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::delete_rs_meta_by_version(const Version& version) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _rs_metas.begin();
    while (it != _rs_metas.end()) {
        if ((*it)->version().first == version.first
              && (*it)->version().second == version.second) {
            _rs_metas.erase(it);
        }
    }

    TabletMetaPB tablet_meta_pb;
    RETURN_NOT_OK(to_tablet_pb_unlock(&tablet_meta_pb));
    _tablet_meta_pb = std::move(tablet_meta_pb);
    RETURN_NOT_OK(save_meta_unlock());

    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::delete_inc_rs_meta_by_version(const Version& version) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _inc_rs_metas.begin();
    while (it != _inc_rs_metas.end()) {
        if ((*it)->version().first == version.first
              && (*it)->version().second == version.second) {
            _inc_rs_metas.erase(it);
        }
    }

    TabletMetaPB tablet_meta_pb;
    RETURN_NOT_OK(to_tablet_pb_unlock(&tablet_meta_pb));
    _tablet_meta_pb = std::move(tablet_meta_pb);
    RETURN_NOT_OK(save_meta_unlock());

    return OLAP_SUCCESS;
}

RowsetMetaSharedPtr TabletMeta::acquire_inc_rs_meta(const Version& version) const {
    std::lock_guard<std::mutex> lock(_mutex);
    RowsetMetaSharedPtr rs_meta = nullptr;
    for (int i = 0; i < _inc_rs_metas.size(); ++i) {
        if (_inc_rs_metas[i]->version().first == version.first
              && _inc_rs_metas[i]->version().second == version.second) {
            rs_meta = _inc_rs_metas[i];
            break;
        }
    }
    return rs_meta;
}

Version TabletMeta::max_version() const {
    std::lock_guard<std::mutex> lock(_mutex);
    Version max_version = { -1, 0 };
    for (auto& rs_meta : _rs_metas) {
        if (rs_meta->end_version() > max_version.second)  {
            max_version = rs_meta->version();
        } else if (rs_meta->end_version() == max_version.second
                && rs_meta->start_version() == max_version.first) {
            max_version = rs_meta->version();
        }
    }
    return max_version;
}

OLAPStatus TabletMeta::add_delete_predicate(
            const DeletePredicatePB& delete_predicate, int64_t version) {
    std::lock_guard<std::mutex> lock(_mutex);
    int ordinal = 0;
    for (auto& del_pred : _del_pred_array) {
        if (del_pred.version() == version) {
            break;
        }
        ordinal++;
    }

    if (ordinal < _del_pred_array.size()) {
        // clear existed predicate
        DeletePredicatePB* del_pred = &(_del_pred_array[ordinal]);
        del_pred->clear_sub_predicates();
        for (const string& predicate : delete_predicate.sub_predicates()) {
            del_pred->add_sub_predicates(predicate);
        }
    } else {
        DeletePredicatePB* del_pred = _del_pred_array.Add();
        del_pred->set_version(version);
        for (const string& predicate : delete_predicate.sub_predicates()) {
            del_pred->add_sub_predicates(predicate);
        }
    }
    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::remove_delete_predicate_by_version(const Version& version) {
    DCHECK(version.first == version.second);
    std::lock_guard<std::mutex> lock(_mutex);
    int ordinal = 0;
    for (; ordinal < _del_pred_array.size(); ++ordinal) {
        const DeletePredicatePB& temp = _del_pred_array.Get(ordinal);
        if (temp.version() == version.first) {
            // log delete condtion
            string del_cond_str;
            const google::protobuf::RepeatedPtrField<string>& sub_predicates = temp.sub_predicates();

            for (int i = 0; i != sub_predicates.size(); ++i) {
                del_cond_str += sub_predicates.Get(i) + ";";
            }

            LOG(INFO) << "remove one del_pred. version=" << temp.version()
                      << ", condition=" << del_cond_str;

            // remove delete condition from PB
            _del_pred_array.SwapElements(ordinal, _del_pred_array.size() - 1);
            _del_pred_array.RemoveLast();
        }
    }
    return OLAP_SUCCESS;
}

DelPredicateArray TabletMeta::delete_predicates() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _del_pred_array;
}

bool TabletMeta::version_for_delete_predicate(const Version& version) {
    if (version.first != version.second) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& del_pred : _del_pred_array) {
        if (del_pred.version() == version.first) {
            return true;
        }
    }

    return false;
}

OLAPStatus TabletMeta::add_alter_task(const AlterTabletTask& alter_task) {
    std::lock_guard<std::mutex> lock(_mutex);
    _alter_task = alter_task;
    RETURN_NOT_OK(_alter_task.to_alter_pb(_tablet_meta_pb.mutable_alter_tablet_task()));
    RETURN_NOT_OK(save_meta_unlock());
    return OLAP_SUCCESS;
}

OLAPStatus TabletMeta::delete_alter_task() {
    std::lock_guard<std::mutex> lock(_mutex);
    _tablet_meta_pb.clear_alter_tablet_task();
    RETURN_NOT_OK(save_meta_unlock());
    _alter_task.clear();
    return OLAP_SUCCESS;
}

void TabletMeta::set_cumulative_layer_point(int32_t new_point) {
    std::lock_guard<std::mutex> lock(_mutex);
    _cumulative_layer_point = new_point;
    TabletMetaPB tablet_meta_pb;
    to_tablet_pb_unlock(&tablet_meta_pb);
    _tablet_meta_pb = std::move(tablet_meta_pb);
}

void TabletMeta::set_shard_id(int32_t shard_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    _shard_id = shard_id;
    TabletMetaPB tablet_meta_pb;
    to_tablet_pb_unlock(&tablet_meta_pb);
    _tablet_meta_pb = std::move(tablet_meta_pb);
}

void TabletMeta::set_creation_time(int64_t creation_time) {
    std::lock_guard<std::mutex> lock(_mutex);
    _creation_time = _creation_time;
    TabletMetaPB tablet_meta_pb;
    to_tablet_pb_unlock(&tablet_meta_pb);
    _tablet_meta_pb = std::move(tablet_meta_pb);
}

}  // namespace doris