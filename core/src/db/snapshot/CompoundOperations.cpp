// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "db/snapshot/CompoundOperations.h"
#include <memory>
#include "db/snapshot/OperationExecutor.h"
#include "db/snapshot/Snapshots.h"

namespace milvus {
namespace engine {
namespace snapshot {

BuildOperation::BuildOperation(const OperationContext& context, ScopedSnapshotT prev_ss) : BaseT(context, prev_ss) {
}
BuildOperation::BuildOperation(const OperationContext& context, ID_TYPE collection_id, ID_TYPE commit_id)
    : BaseT(context, collection_id, commit_id) {
}

bool
BuildOperation::PreExecute(Store& store) {
    SegmentCommitOperation op(context_, prev_ss_);
    op(store);
    context_.new_segment_commit = op.GetResource();
    if (!context_.new_segment_commit)
        return false;

    PartitionCommitOperation pc_op(context_, prev_ss_);
    pc_op(store);

    OperationContext cc_context;
    cc_context.new_partition_commit = pc_op.GetResource();
    CollectionCommitOperation cc_op(cc_context, prev_ss_);
    cc_op(store);

    for (auto& new_segment_file : context_.new_segment_files) {
        AddStep(*new_segment_file);
    }
    AddStep(*context_.new_segment_commit);
    AddStep(*pc_op.GetResource());
    AddStep(*cc_op.GetResource());
    return true;
}

bool
BuildOperation::DoExecute(Store& store) {
    if (status_ != OP_PENDING) {
        return false;
    }
    if (IsStale()) {
        status_ = OP_STALE_CANCEL;
        return false;
    }
    /* if (!prev_ss_->HasFieldElement(context_.field_name, context_.field_element_name)) { */
    /*     status_ = OP_FAIL_INVALID_PARAMS; */
    /*     return false; */
    /* } */

    // PXU TODO: Temp comment below check for test
    /* if (prev_ss_->HasSegmentFile(context_.field_name, context_.field_element_name, context_.segment_id)) { */
    /*     status_ = OP_FAIL_DUPLICATED; */
    /*     return; */
    /* } */

    if (IsStale()) {
        status_ = OP_STALE_CANCEL;
        // PXU TODO: Produce cleanup job
        return false;
    }
    std::any_cast<SegmentFilePtr>(steps_[0])->Activate();
    std::any_cast<SegmentCommitPtr>(steps_[1])->Activate();
    std::any_cast<PartitionCommitPtr>(steps_[2])->Activate();
    std::any_cast<CollectionCommitPtr>(steps_[3])->Activate();
    return true;
}

SegmentFilePtr
BuildOperation::CommitNewSegmentFile(const SegmentFileContext& context) {
    auto new_sf_op = std::make_shared<SegmentFileOperation>(context, prev_ss_);
    OperationExecutor::GetInstance().Submit(new_sf_op);
    new_sf_op->WaitToFinish();
    context_.new_segment_files.push_back(new_sf_op->GetResource());
    return new_sf_op->GetResource();
}

NewSegmentOperation::NewSegmentOperation(const OperationContext& context, ScopedSnapshotT prev_ss)
    : BaseT(context, prev_ss) {
}
NewSegmentOperation::NewSegmentOperation(const OperationContext& context, ID_TYPE collection_id, ID_TYPE commit_id)
    : BaseT(context, collection_id, commit_id) {
}

bool
NewSegmentOperation::DoExecute(Store& store) {
    auto i = 0;
    for (; i < context_.new_segment_files.size(); ++i) {
        std::any_cast<SegmentFilePtr>(steps_[i])->Activate();
    }
    std::any_cast<SegmentPtr>(steps_[i++])->Activate();
    std::any_cast<SegmentCommitPtr>(steps_[i++])->Activate();
    std::any_cast<PartitionCommitPtr>(steps_[i++])->Activate();
    std::any_cast<CollectionCommitPtr>(steps_[i++])->Activate();
    return true;
}

bool
NewSegmentOperation::PreExecute(Store& store) {
    // PXU TODO:
    // 1. Check all requried field elements have related segment files
    // 2. Check Stale and others
    SegmentCommitOperation op(context_, prev_ss_);
    op(store);
    context_.new_segment_commit = op.GetResource();
    if (!context_.new_segment_commit)
        return false;

    PartitionCommitOperation pc_op(context_, prev_ss_);
    pc_op(store);

    OperationContext cc_context;
    cc_context.new_partition_commit = pc_op.GetResource();
    CollectionCommitOperation cc_op(cc_context, prev_ss_);
    cc_op(store);

    for (auto& new_segment_file : context_.new_segment_files) {
        AddStep(*new_segment_file);
    }
    AddStep(*context_.new_segment);
    AddStep(*context_.new_segment_commit);
    AddStep(*pc_op.GetResource());
    AddStep(*cc_op.GetResource());
    return true;
}

SegmentPtr
NewSegmentOperation::CommitNewSegment() {
    auto op = std::make_shared<SegmentOperation>(context_, prev_ss_);
    OperationExecutor::GetInstance().Submit(op);
    op->WaitToFinish();
    context_.new_segment = op->GetResource();
    return context_.new_segment;
}

SegmentFilePtr
NewSegmentOperation::CommitNewSegmentFile(const SegmentFileContext& context) {
    auto c = context;
    c.segment_id = context_.new_segment->GetID();
    c.partition_id = context_.new_segment->GetPartitionId();
    auto new_sf_op = std::make_shared<SegmentFileOperation>(c, prev_ss_);
    OperationExecutor::GetInstance().Submit(new_sf_op);
    new_sf_op->WaitToFinish();
    context_.new_segment_files.push_back(new_sf_op->GetResource());
    return new_sf_op->GetResource();
}

MergeOperation::MergeOperation(const OperationContext& context, ScopedSnapshotT prev_ss) : BaseT(context, prev_ss) {
}
MergeOperation::MergeOperation(const OperationContext& context, ID_TYPE collection_id, ID_TYPE commit_id)
    : BaseT(context, collection_id, commit_id) {
}

SegmentPtr
MergeOperation::CommitNewSegment() {
    if (context_.new_segment)
        return context_.new_segment;
    auto op = std::make_shared<SegmentOperation>(context_, prev_ss_);
    OperationExecutor::GetInstance().Submit(op);
    op->WaitToFinish();
    context_.new_segment = op->GetResource();
    return context_.new_segment;
}

SegmentFilePtr
MergeOperation::CommitNewSegmentFile(const SegmentFileContext& context) {
    // PXU TODO: Check element type and segment file mapping rules
    auto new_segment = CommitNewSegment();
    auto c = context;
    c.segment_id = new_segment->GetID();
    c.partition_id = new_segment->GetPartitionId();
    auto new_sf_op = std::make_shared<SegmentFileOperation>(c, prev_ss_);
    OperationExecutor::GetInstance().Submit(new_sf_op);
    new_sf_op->WaitToFinish();
    context_.new_segment_files.push_back(new_sf_op->GetResource());
    return new_sf_op->GetResource();
}

bool
MergeOperation::PreExecute(Store& store) {
    // PXU TODO:
    // 1. Check all requried field elements have related segment files
    // 2. Check Stale and others
    SegmentCommitOperation op(context_, prev_ss_);
    op(store);
    context_.new_segment_commit = op.GetResource();
    if (!context_.new_segment_commit)
        return false;

    // PXU TODO: Check stale segments

    PartitionCommitOperation pc_op(context_, prev_ss_);
    pc_op(store);

    OperationContext cc_context;
    cc_context.new_partition_commit = pc_op.GetResource();
    CollectionCommitOperation cc_op(cc_context, prev_ss_);
    cc_op(store);

    for (auto& new_segment_file : context_.new_segment_files) {
        AddStep(*new_segment_file);
    }
    AddStep(*context_.new_segment);
    AddStep(*context_.new_segment_commit);
    AddStep(*pc_op.GetResource());
    AddStep(*cc_op.GetResource());
    return true;
}

bool
MergeOperation::DoExecute(Store& store) {
    auto i = 0;
    for (; i < context_.new_segment_files.size(); ++i) {
        std::any_cast<SegmentFilePtr>(steps_[i])->Activate();
    }
    std::any_cast<SegmentPtr>(steps_[i++])->Activate();
    std::any_cast<SegmentCommitPtr>(steps_[i++])->Activate();
    std::any_cast<PartitionCommitPtr>(steps_[i++])->Activate();
    std::any_cast<CollectionCommitPtr>(steps_[i++])->Activate();
    return true;
}

GetSnapshotIDsOperation::GetSnapshotIDsOperation(ID_TYPE collection_id, bool reversed)
    : BaseT(OperationContext(), ScopedSnapshotT()), collection_id_(collection_id), reversed_(reversed) {
}

bool
GetSnapshotIDsOperation::DoExecute(Store& store) {
    ids_ = store.AllActiveCollectionCommitIds(collection_id_, reversed_);
    return true;
}

const IDS_TYPE&
GetSnapshotIDsOperation::GetIDs() const {
    return ids_;
}

GetCollectionIDsOperation::GetCollectionIDsOperation(bool reversed)
    : BaseT(OperationContext(), ScopedSnapshotT()), reversed_(reversed) {
}

bool
GetCollectionIDsOperation::DoExecute(Store& store) {
    ids_ = store.AllActiveCollectionIds(reversed_);
    return true;
}

const IDS_TYPE&
GetCollectionIDsOperation::GetIDs() const {
    return ids_;
}

CreateCollectionOperation::CreateCollectionOperation(const CreateCollectionContext& context)
    : BaseT(OperationContext(), ScopedSnapshotT()), context_(context) {
}

bool
CreateCollectionOperation::DoExecute(Store& store) {
    // TODO: Do some checks
    auto collection = store.CreateCollection(Collection(context_.collection->GetName()));
    AddStep(*collection);
    MappingT field_commit_ids = {};
    auto field_idx = 0;
    for (auto& field_kv : context_.fields_schema) {
        field_idx++;
        auto& field_schema = field_kv.first;
        auto& field_elements = field_kv.second;
        auto field = store.CreateResource<Field>(Field(field_schema->GetName(), field_idx));
        AddStep(*field);
        MappingT element_ids = {};
        auto raw_element = store.CreateResource<FieldElement>(
            FieldElement(collection->GetID(), field->GetID(), "RAW", FieldElementType::RAW));
        AddStep(*raw_element);
        element_ids.insert(raw_element->GetID());
        for (auto& element_schema : field_elements) {
            auto element = store.CreateResource<FieldElement>(FieldElement(
                collection->GetID(), field->GetID(), element_schema->GetName(), element_schema->GetFtype()));
            AddStep(*element);
            element_ids.insert(element->GetID());
        }
        auto field_commit =
            store.CreateResource<FieldCommit>(FieldCommit(collection->GetID(), field->GetID(), element_ids));
        AddStep(*field_commit);
        field_commit_ids.insert(field_commit->GetID());
    }
    auto schema_commit = store.CreateResource<SchemaCommit>(SchemaCommit(collection->GetID(), field_commit_ids));
    AddStep(*schema_commit);
    auto partition = store.CreateResource<Partition>(Partition("_default", collection->GetID()));
    AddStep(*partition);
    auto partition_commit =
        store.CreateResource<PartitionCommit>(PartitionCommit(collection->GetID(), partition->GetID()));
    AddStep(*partition_commit);
    auto collection_commit = store.CreateResource<CollectionCommit>(
        CollectionCommit(collection->GetID(), schema_commit->GetID(), {partition_commit->GetID()}));
    AddStep(*collection_commit);
    context_.collection_commit = collection_commit;
    return true;
}

ScopedSnapshotT
CreateCollectionOperation::GetSnapshot() const {
    if (ids_.size() == 0)
        return ScopedSnapshotT();
    if (!context_.collection_commit)
        return ScopedSnapshotT();
    return Snapshots::GetInstance().GetSnapshot(context_.collection_commit->GetCollectionId());
}

bool
SoftDeleteCollectionOperation::DoExecute(Store& store) {
    if (!context_.collection) {
        status_ = Status(40006, "Invalid Context");
        return false;
    }
    context_.collection->Deactivate();
    AddStep(*context_.collection);
    status_ = Status::OK();
    return true;
}

}  // namespace snapshot
}  // namespace engine
}  // namespace milvus
