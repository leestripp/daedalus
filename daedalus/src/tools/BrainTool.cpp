// SPDX-License-Identifier: BSL-1.0
// src/tools/BrainTool.cpp — see BrainTool.h for the action catalogue.

#include "BrainTool.h"
#include "../core/FileIO.h"
#include "../core/SecondBrain.h"

#include <stdexcept>
#include <string>
#include <vector>

BrainTool::BrainTool() {
    name_ = "brain";
    description_ =
        "Your Global Second Brain — a plain-markdown knowledge base "
        "GLOBAL across all projects, shared by every agent on this "
        "machine. Use it to capture, organise, find and connect "
        "long-term knowledge: research, decisions, how-tos, reference "
        "material — things worth keeping beyond this session (use "
        "'remember' for short session-scoped facts instead). Notes live "
        "in PARA folders: inbox (quick captures), projects (active "
        "goals), areas (ongoing responsibilities), resources (reference), "
        "archives (done/inactive). One idea per note; link notes with "
        "[[note-id]] wiki-links in the body. Actions: 'capture' (new "
        "note), 'search' (full-text), 'read', 'list', 'update' (replace "
        "title/tags/body), 'append' (add to body), 'file' (move to "
        "another folder), 'backlinks' (what links here), 'delete', "
        "'reindex' (rebuild the search index from disk).";
}

json BrainTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"action", {
                        {"type", "string"},
                        {"enum", {"capture", "search", "read", "list",
                                  "update", "append", "file", "backlinks",
                                  "delete", "reindex"}},
                        {"description", "capture: new note (body required). search: full-text (query required). read: full note (id required). list: metadata rows. update: replace given fields (id required). append: add markdown to body (id required). file: move to folder (id+folder required). backlinks: notes linking to id. delete: remove note (id required). reindex: rebuild the index from the markdown files."}
                    }},
                    {"folder", {
                        {"type", "string"},
                        {"enum", {"inbox", "projects", "areas",
                                  "resources", "archives"}},
                        {"description", "PARA folder. capture: destination (default 'inbox'). list: filter. file: destination."}
                    }},
                    {"title", {
                        {"type", "string"},
                        {"description", "Note title (capture/update). capture: falls back to the body's first heading."}
                    }},
                    {"tags", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Tag list (capture/update). Short lowercase slugs, e.g. [\"pkm\", \"cpp\"]."}
                    }},
                    {"body", {
                        {"type", "string"},
                        {"description", "Markdown body (capture: required; update: replace). Link notes with [[note-id]]."}
                    }},
                    {"markdown", {
                        {"type", "string"},
                        {"description", "Markdown appended to the body (append, required)."}
                    }},
                    {"id", {
                        {"type", "string"},
                        {"description", "Note id (slug), as returned by capture/list/search. Required for read/update/append/file/backlinks/delete."}
                    }},
                    {"query", {
                        {"type", "string"},
                        {"description", "Plain-language search text (search, required). Matches title/tags/body with prefix + stemming."}
                    }},
                    {"tag", {
                        {"type", "string"},
                        {"description", "Single tag filter (list)."}
                    }},
                    {"limit", {
                        {"type", "integer"},
                        {"description", "Max results (search default 20, list default 100)."}
                    }}
                }},
                {"required", {"action"}}
            }}
        }}
    };
}

json BrainTool::execute(const json& args, FileIO& /*file_io*/) {
    const std::string action = args.value("action", "");
    try {
        auto& brain = daedalus::SecondBrain::global();
        if (action == "capture")   return do_capture(brain, args);
        if (action == "search")    return do_search(brain, args);
        if (action == "read")      return do_read(brain, args);
        if (action == "list")      return do_list(brain, args);
        if (action == "update")    return do_update(brain, args);
        if (action == "append")    return do_append(brain, args);
        if (action == "file")      return do_file(brain, args);
        if (action == "backlinks") return do_backlinks(brain, args);
        if (action == "delete")    return do_delete(brain, args);
        if (action == "reindex")   return do_reindex(brain);
        return json::object({
            {"status", "error"},
            {"message", "brain: unknown or missing action '" + action +
                        "' — expected capture | search | read | list | "
                        "update | append | file | backlinks | delete | "
                        "reindex"},
        });
    } catch (const std::exception& e) {
        // FS/DB/argument errors surface as tool errors, never as crashes
        // (same containment discipline as VaultTool).
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string require_string(const json& args, const char* key,
                           const char* action) {
    const auto v = args.value(key, "");
    if (v.empty())
        throw std::invalid_argument(
            std::string("brain: '") + key + "' is required for action '" +
            action + "'");
    return v;
}

std::vector<std::string> string_array(const json& args, const char* key) {
    std::vector<std::string> out;
    if (!args.contains(key) || !args.at(key).is_array()) return out;
    for (const auto& item : args.at(key)) {
        if (item.is_string()) {
            const auto s = item.get<std::string>();
            if (!s.empty()) out.push_back(s);
        }
    }
    return out;
}

json note_json(const daedalus::BrainNote& n) {
    return json::object({
        {"id", n.id},
        {"folder", n.folder},
        {"title", n.title},
        {"tags", n.tags},
        {"created", n.created},
        {"updated", n.updated},
        {"links", n.links},
        {"body", n.body},
    });
}

}  // anonymous namespace

json BrainTool::meta_json(const daedalus::SettingsStore::BrainNoteRow& m) {
    return json::object({
        {"id", m.id},
        {"folder", m.folder},
        {"title", m.title},
        {"tags", m.tags},
        {"created", m.created},
        {"updated", m.updated},
    });
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------

json BrainTool::do_capture(daedalus::SecondBrain& brain, const json& args) {
    const std::string body = require_string(args, "body", "capture");
    const std::string folder = args.value("folder", "inbox");
    const auto note = brain.capture(folder,
                                    args.value("title", ""),
                                    string_array(args, "tags"),
                                    body);
    return json::object({
        {"status", "ok"},
        {"id", note.id},
        {"folder", note.folder},
        {"title", note.title},
        {"message", "captured as " + note.folder + "/" + note.id},
    });
}

json BrainTool::do_search(daedalus::SecondBrain& brain, const json& args) {
    const std::string query = require_string(args, "query", "search");
    const int limit = args.value("limit", 20);
    const auto hits = brain.search(query, limit);

    json results = json::array();
    for (const auto& h : hits) {
        auto j = meta_json(h.note);
        j["snippet"] = h.snippet;
        results.push_back(std::move(j));
    }
    return json::object({
        {"status", "ok"},
        {"count", results.size()},
        {"results", results},
    });
}

json BrainTool::do_read(daedalus::SecondBrain& brain, const json& args) {
    const std::string id = require_string(args, "id", "read");
    const auto note = brain.read(id);
    if (!note.has_value())
        return json::object({
            {"status", "error"},
            {"message", "brain: no note with id '" + id + "'"},
        });
    auto j = note_json(*note);
    j["status"] = "ok";
    j["backlinks"] = brain.backlinks(id);
    return j;
}

json BrainTool::do_list(daedalus::SecondBrain& brain, const json& args) {
    const int limit = args.value("limit", 100);
    const auto rows = brain.list(args.value("folder", ""),
                                 args.value("tag", ""),
                                 limit);
    json notes = json::array();
    for (const auto& r : rows) notes.push_back(meta_json(r));
    return json::object({
        {"status", "ok"},
        {"count", notes.size()},
        {"folders", daedalus::SecondBrain::folders()},
        {"notes", notes},
    });
}

json BrainTool::do_update(daedalus::SecondBrain& brain, const json& args) {
    const std::string id = require_string(args, "id", "update");

    std::optional<std::string> title;
    if (args.contains("title") && args.at("title").is_string())
        title = args.at("title").get<std::string>();
    std::optional<std::vector<std::string>> tags;
    if (args.contains("tags") && args.at("tags").is_array())
        tags = string_array(args, "tags");
    std::optional<std::string> body;
    if (args.contains("body") && args.at("body").is_string())
        body = args.at("body").get<std::string>();

    if (!title.has_value() && !tags.has_value() && !body.has_value())
        return json::object({
            {"status", "error"},
            {"message", "brain: update needs at least one of "
                        "title/tags/body"},
        });

    if (!brain.update(id, title, tags, body))
        return json::object({
            {"status", "error"},
            {"message", "brain: no note with id '" + id + "'"},
        });
    return json::object({{"status", "ok"}, {"id", id}});
}

json BrainTool::do_append(daedalus::SecondBrain& brain, const json& args) {
    const std::string id = require_string(args, "id", "append");
    const std::string md = require_string(args, "markdown", "append");
    if (!brain.append(id, md))
        return json::object({
            {"status", "error"},
            {"message", "brain: no note with id '" + id + "'"},
        });
    return json::object({{"status", "ok"}, {"id", id}});
}

json BrainTool::do_file(daedalus::SecondBrain& brain, const json& args) {
    const std::string id = require_string(args, "id", "file");
    const std::string folder = require_string(args, "folder", "file");
    if (!brain.move(id, folder))
        return json::object({
            {"status", "error"},
            {"message", "brain: no note with id '" + id + "'"},
        });
    return json::object({
        {"status", "ok"},
        {"id", id},
        {"folder", folder},
        {"message", "filed " + id + " under " + folder},
    });
}

json BrainTool::do_backlinks(daedalus::SecondBrain& brain, const json& args) {
    const std::string id = require_string(args, "id", "backlinks");
    const auto refs = brain.backlinks(id);
    return json::object({
        {"status", "ok"},
        {"id", id},
        {"count", refs.size()},
        {"backlinks", refs},
    });
}

json BrainTool::do_delete(daedalus::SecondBrain& brain, const json& args) {
    const std::string id = require_string(args, "id", "delete");
    if (!brain.remove(id))
        return json::object({
            {"status", "error"},
            {"message", "brain: no note with id '" + id + "'"},
        });
    return json::object({{"status", "ok"}, {"deleted", id}});
}

json BrainTool::do_reindex(daedalus::SecondBrain& brain) {
    const auto r = brain.reindex();
    return json::object({
        {"status", "ok"},
        {"indexed", r.indexed},
        {"removed", r.removed},
        {"message", "reindex complete: " + std::to_string(r.indexed) +
                    " (re)indexed, " + std::to_string(r.removed) +
                    " removed"},
    });
}
