//
// Created by wastl on 15.11.15.
//

#include "service.h"

#include <unordered_set>
#include <model/rdf_operators.h>

using grpc::Status;
using grpc::StatusCode;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using google::protobuf::Int64Value;
using google::protobuf::Message;
using google::protobuf::Empty;
using marmotta::rdf::proto::Statement;
using marmotta::rdf::proto::Namespace;
using marmotta::rdf::proto::Resource;
using marmotta::service::proto::ContextRequest;

namespace marmotta {
namespace service {

// A STL iterator wrapper around a client reader.
template <class Proto>
class ReaderIterator : public persistence::Iterator<Proto> {
 public:
    ReaderIterator() : finished(true) { }

    ReaderIterator(grpc::ServerReader<Proto>* r) : reader(r), finished(false) {
        // Immediately move to first element.
        operator++();
    }

    persistence::Iterator<Proto>& operator++() override {
        if (!finished) {
            finished = !reader->Read(&buffer);
        }
        return *this;
    }

    Proto& operator*() override {
        return buffer;
    }

    Proto* operator->() override {
        return &buffer;
    }

    bool operator==(const persistence::Iterator<Proto>& other) override {
        return finished == static_cast<const ReaderIterator<Proto>&>(other).finished;
    }

    bool operator!=(const persistence::Iterator<Proto>& other) override {
        return finished != static_cast<const ReaderIterator<Proto>&>(other).finished;
    }

    static ReaderIterator<Proto> end() {
        return ReaderIterator<Proto>();
    }

 private:
    grpc::ServerReader<Proto>* reader;
    Proto buffer;
    bool finished;
};

typedef ReaderIterator<rdf::proto::Statement> StatementIterator;
typedef ReaderIterator<rdf::proto::Namespace> NamespaceIterator;
typedef ReaderIterator<service::proto::UpdateRequest> UpdateIterator;


Status LevelDBService::AddNamespaces(
        ServerContext* context, ServerReader<Namespace>* reader, Int64Value* result) {

    auto begin = NamespaceIterator(reader);
    auto end   = NamespaceIterator::end();
    int64_t count = persistence.AddNamespaces(begin, end);
    result->set_value(count);

    return Status::OK;
}

grpc::Status LevelDBService::GetNamespace(
        ServerContext *context, const rdf::proto::Namespace *pattern, Namespace *result) {

    Status status(StatusCode::NOT_FOUND, "Namespace not found");
    persistence.GetNamespaces(*pattern, [&result, &status](const Namespace &r) {
        *result = r;
        status = Status::OK;
    });

    return status;
}

grpc::Status LevelDBService::GetNamespaces(
        ServerContext *context, const Empty *ignored, ServerWriter<Namespace> *result) {

    Namespace pattern; // empty pattern
    persistence.GetNamespaces(pattern, [&result](const Namespace &r) {
        result->Write(r);
    });

    return Status::OK;
}


Status LevelDBService::AddStatements(
        ServerContext* context, ServerReader<Statement>* reader, Int64Value* result) {

    auto begin = StatementIterator(reader);
    auto end   = StatementIterator::end();
    int64_t count = persistence.AddStatements(begin, end);
    result->set_value(count);

    return Status::OK;
}


Status LevelDBService::GetStatements(
        ServerContext* context, const Statement* pattern, ServerWriter<Statement>* result) {

    persistence.GetStatements(*pattern, [&result](const Statement& stmt) {
        result->Write(stmt);
    });

    return Status::OK;
}

Status LevelDBService::RemoveStatements(
        ServerContext* context, const Statement* pattern, Int64Value* result) {

    int64_t count = persistence.RemoveStatements(*pattern);
    result->set_value(count);

    return Status::OK;
}

Status LevelDBService::Clear(
        ServerContext* context, const ContextRequest* contexts, Int64Value* result) {


    int64_t count = 0;

    Statement pattern;
    if (contexts->context_size() > 0) {
        for (const Resource &r : contexts->context()) {
            pattern.mutable_context()->CopyFrom(r);
            count += persistence.RemoveStatements(pattern);
        }
    } else {
        count += persistence.RemoveStatements(pattern);
    }
    result->set_value(count);

    return Status::OK;
}

Status LevelDBService::Size(
        ServerContext* context, const ContextRequest* contexts, Int64Value* result) {

    int64_t count = 0;

    if (contexts->context_size() > 0) {
        Statement pattern;
        for (const Resource &r : contexts->context()) {
            pattern.mutable_context()->CopyFrom(r);

            persistence.GetStatements(pattern, [&count](const Statement& stmt) {
                count++;
            });
        }
    } else {
        Statement pattern;

        persistence.GetStatements(pattern, [&count](const Statement& stmt) {
            count++;
        });
    }
    result->set_value(count);

    return Status::OK;

}


grpc::Status LevelDBService::GetContexts(
        ServerContext *context, const Empty *ignored, ServerWriter<Resource> *result) {
    // Currently we need to iterate over all statements and collect the results.
    Statement pattern;
    std::unordered_set<Resource> contexts;

    persistence.GetStatements(pattern, [&contexts](const Statement& stmt) {
        if (stmt.has_context()) {
            contexts.insert(stmt.context());
        }
    });

    for (auto c : contexts) {
        result->Write(c);
    }
    return Status::OK;
}

grpc::Status LevelDBService::Update(grpc::ServerContext *context,
                                    grpc::ServerReader<service::proto::UpdateRequest> *reader,
                                    service::proto::UpdateResponse *result) {

    auto begin = UpdateIterator(reader);
    auto end   = UpdateIterator::end();
    persistence::UpdateStatistics stats = persistence.Update(begin, end);

    result->set_added_namespaces(stats.added_ns);
    result->set_removed_namespaces(stats.removed_ns);
    result->set_added_statements(stats.added_stmts);
    result->set_removed_statements(stats.removed_stmts);

    return Status::OK;
}
}  // namespace service
}  // namespace marmotta