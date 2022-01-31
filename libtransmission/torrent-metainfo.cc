// This file Copyright © 2007-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0), GPLv3 (SPDX: GPL-3.0),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cctype>
#include <iterator>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include <event2/util.h> // evutil_ascii_strncasecmp

#include "transmission.h"

#include "benc.h"
#include "crypto-utils.h"
#include "error-types.h"
#include "error.h"
#include "file.h"
#include "log.h"
#include "quark.h"
#include "torrent-metainfo.h"
#include "tr-assert.h"
#include "utils.h"
#include "web-utils.h"
// #include "variant.h"

using namespace std::literals;

//// C BINDINGS

#if 0
/// Lifecycle

tr_torrent_metainfo* tr_torrentMetainfoNewFromData(char const* data, size_t data_len, struct tr_error** error)
{
    auto* tm = new tr_torrent_metainfo{};
    if (!tm->parseBenc(std::string_view{ data, data_len }, error))
    {
        delete tm;
        return nullptr;
    }

    return tm;
}

tr_torrent_metainfo* tr_torrentMetainfoNewFromFile(char const* filename, struct tr_error** error)
{
    auto* tm = new tr_torrent_metainfo{};
    if (!tm->parseBencFromFile(filename ? filename : "", nullptr, error))
    {
        delete tm;
        return nullptr;
    }

    return tm;
}

void tr_torrentMetainfoFree(tr_torrent_metainfo* tm)
{
    delete tm;
}

////  Accessors

char* tr_torrentMetainfoMagnet(struct tr_torrent_metainfo const* tm)
{
    return tr_strvDup(tm->magnet());
}

/// Info

tr_torrent_metainfo_info* tr_torrentMetainfoGet(tr_torrent_metainfo const* tm, tr_torrent_metainfo_info* setme)
{
    setme->comment = tm->comment.c_str();
    setme->creator = tm->creator.c_str();
    setme->info_hash = tm->info_hash;
    setme->info_hash_string = std::data(tm->info_hash_chars);
    setme->is_private = tm->is_private;
    setme->n_pieces = tm->n_pieces;
    setme->name = tm->name.c_str();
    setme->source = tm->source.c_str();
    setme->time_created = tm->time_created;
    setme->total_size = tm->total_size;
    return setme;
}

/// Files

size_t tr_torrentMetainfoFileCount(tr_torrent_metainfo const* tm)
{
    return std::size(tm->files);
}

tr_torrent_metainfo_file_info* tr_torrentMetainfoFile(
    tr_torrent_metainfo const* tm,
    size_t n,
    tr_torrent_metainfo_file_info* setme)
{
    auto& file = tm->files[n];
    setme->path = file.path.c_str();
    setme->size = file.size;
    return setme;
}

/// Trackers

size_t tr_torrentMetainfoTrackerCount(tr_torrent_metainfo const* tm)
{
    return std::size(tm->trackers);
}

tr_torrent_metainfo_tracker_info* tr_torrentMetainfoTracker(
    tr_torrent_metainfo const* tm,
    size_t n,
    tr_torrent_metainfo_tracker_info* setme)
{
    auto it = std::begin(tm->trackers);
    std::advance(it, n);
    auto const& tracker = it->second;
    setme->announce_url = tr_quark_get_string(tracker.announce_url);
    setme->scrape_url = tr_quark_get_string(tracker.scrape_url);
    setme->tier = tracker.tier;
    return setme;
}
#endif

/***
****
***/

/**
 * @brief Ensure that the URLs for multfile torrents end in a slash.
 *
 * See http://bittorrent.org/beps/bep_0019.html#metadata-extension
 * for background on how the trailing slash is used for "url-list"
 * fields.
 *
 * This function is to workaround some .torrent generators, such as
 * mktorrent and very old versions of utorrent, that don't add the
 * trailing slash for multifile torrents if omitted by the end user.
 */
std::string tr_torrent_metainfo::fixWebseedUrl(tr_torrent_metainfo const& tm, std::string_view url)
{
    url = tr_strvStrip(url);

    if (std::size(tm.files_) > 1 && !std::empty(url) && url.back() != '/')
    {
        return std::string{ url } + '/';
    }

    return std::string{ url };
}

#if 0
void tr_torrent_metainfo::parseWebseeds(tr_torrent_metainfo& setme, tr_variant* meta)
{
    setme.webseed_urls_.clear();

    auto url = std::string_view{};
    tr_variant* urls = nullptr;
    if (tr_variantDictFindList(meta, TR_KEY_url_list, &urls))
    {
        size_t const n = tr_variantListSize(urls);
        setme.webseed_urls_.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (tr_variantGetStrView(tr_variantListChild(urls, i), &url) && tr_urlIsValid(url))
            {
                setme.webseed_urls_.push_back(fixWebseedUrl(setme, url));
            }
        }
    }
    else if (tr_variantDictFindStrView(meta, TR_KEY_url_list, &url) && tr_urlIsValid(url)) // handle single items in webseeds
    {
        setme.webseed_urls_.push_back(fixWebseedUrl(setme, url));
    }
}
#endif

static std::string sanitizeToken(std::string_view in)
{
    // auto const original_out_len = std::size(out);
    // auto const original_in = in;

    // remove leading spaces
    auto constexpr leading_test = [](unsigned char ch)
    {
        return isspace(ch);
    };
    auto const it = std::find_if_not(std::begin(in), std::end(in), leading_test);
    in.remove_prefix(std::distance(std::begin(in), it));

    // remove trailing spaces and '.'
    auto constexpr trailing_test = [](unsigned char ch)
    {
        return isspace(ch) || ch == '.';
    };
    auto const rit = std::find_if_not(std::rbegin(in), std::rend(in), trailing_test);
    in.remove_suffix(std::distance(std::rbegin(in), rit));

    // munge banned characters
    // https://docs.microsoft.com/en-us/windows/desktop/FileIO/naming-a-file
    auto constexpr ensure_legal_char = [](auto ch)
    {
        auto constexpr Banned = std::string_view{ "<>:\"/\\|?*" };
        auto const banned = Banned.find(ch) != std::string_view::npos || (unsigned char)ch < 0x20;
        return banned ? '_' : ch;
    };
    auto out = std::string{};
    std::transform(std::begin(in), std::end(in), std::back_inserter(out), ensure_legal_char);

    // munge banned filenames
    // https://docs.microsoft.com/en-us/windows/desktop/FileIO/naming-a-file
    auto constexpr ReservedNames = std::array<std::string_view, 22>{
        "CON"sv,  "PRN"sv,  "AUX"sv,  "NUL"sv,  "COM1"sv, "COM2"sv, "COM3"sv, "COM4"sv, "COM5"sv, "COM6"sv, "COM7"sv,
        "COM8"sv, "COM9"sv, "LPT1"sv, "LPT2"sv, "LPT3"sv, "LPT4"sv, "LPT5"sv, "LPT6"sv, "LPT7"sv, "LPT8"sv, "LPT9"sv,
    };
    if (std::find(std::begin(ReservedNames), std::end(ReservedNames), out) != std::end(ReservedNames))
    {
        out.insert(std::begin(out), '_');
    }

    std::cerr << __FILE__ << ':' << __LINE__ << " sanitize in [" << in << "] out [" << out << ']' << std::endl;
    return out;
}

#if 0
bool tr_torrent_metainfo::parsePath(std::string_view root, tr_variant* path, std::string& setme)
{
    if (!tr_variantIsList(path))
    {
        return false;
    }

    setme = root;
    for (size_t i = 0, n = tr_variantListSize(path); i < n; ++i)
    {
        auto raw = std::string_view{};
        if (!tr_variantGetStrView(tr_variantListChild(path, i), &raw))
        {
            return false;
        }

        auto is_component_adjusted = bool{};
        auto const pos = std::size(setme);
        if (!appendSanitizedComponent(setme, raw, &is_component_adjusted))
        {
            continue;
        }

        setme.insert(std::begin(setme) + pos, TR_PATH_DELIMITER);
    }

    if (std::size(setme) <= std::size(root))
    {
        return false;
    }

    tr_strvUtf8Clean(setme, setme);
    return true;
}

std::string_view tr_torrent_metainfo::parseFiles(tr_torrent_metainfo& setme, tr_variant* info_dict, uint64_t* setme_total_size)
{
    auto is_root_adjusted = bool{ false };
    auto root_name = std::string{};
    auto total_size = uint64_t{ 0 };

    setme.files_.clear();

    if (!appendSanitizedComponent(root_name, setme.name_, &is_root_adjusted))
    {
        return "invalid name"sv;
    }

    // bittorrent 1.0 spec
    // http://bittorrent.org/beps/bep_0003.html
    //
    // "There is also a key length or a key files, but not both or neither.
    //
    // "If length is present then the download represents a single file,
    // otherwise it represents a set of files which go in a directory structure.
    // In the single file case, length maps to the length of the file in bytes.
    auto len = int64_t{};
    tr_variant* files_entry = nullptr;
    if (tr_variantDictFindInt(info_dict, TR_KEY_length, &len))
    {
        total_size = len;
        setme.files_.emplace_back(root_name, len);
    }

    // "For the purposes of the other keys, the multi-file case is treated as
    // only having a single file by concatenating the files in the order they
    // appear in the files list. The files list is the value files maps to,
    // and is a list of dictionaries containing the following keys:
    // length - The length of the file, in bytes.
    // path - A list of UTF-8 encoded strings corresponding to subdirectory
    // names, the last of which is the actual file name (a zero length list
    // is an error case).
    // In the multifile case, the name key is the name of a directory.
    else if (tr_variantDictFindList(info_dict, TR_KEY_files, &files_entry))
    {
        auto buf = std::string{};
        buf.reserve(1024); // arbitrary
        auto const n_files = size_t{ tr_variantListSize(files_entry) };
        setme.files_.reserve(n_files);
        for (size_t i = 0; i < n_files; ++i)
        {
            auto* const file_entry = tr_variantListChild(files_entry, i);
            if (!tr_variantIsDict(file_entry))
            {
                return "'files' is not a dictionary";
            }

            if (!tr_variantDictFindInt(file_entry, TR_KEY_length, &len))
            {
                return "length";
            }

            tr_variant* path_variant = nullptr;
            if (!tr_variantDictFindList(file_entry, TR_KEY_path_utf_8, &path_variant) &&
                !tr_variantDictFindList(file_entry, TR_KEY_path, &path_variant))
            {
                return "path";
            }

            if (!parsePath(root_name, path_variant, buf))
            {
                return "path";
            }

            setme.files_.emplace_back(buf, len);
            total_size += len;
        }
    }
    else
    {
        // TODO: add support for 'file tree' BitTorrent 2 torrents / hybrid torrents.
        // Patches welcomed!
        // https://www.bittorrent.org/beps/bep_0052.html#info-dictionary
        return "'info' dict has neither 'files' nor 'length' key";
    }

    *setme_total_size = total_size;
    return {};
}

// https://www.bittorrent.org/beps/bep_0012.html
std::string_view tr_torrent_metainfo::parseAnnounce(tr_torrent_metainfo& setme, tr_variant* meta)
{
    setme.announce_list_.clear();

    auto url = std::string_view{};

    // announce-list
    // example: d['announce-list'] = [ [tracker1], [backup1], [backup2] ]
    if (tr_variant* tiers = nullptr; tr_variantDictFindList(meta, TR_KEY_announce_list, &tiers))
    {
        for (size_t i = 0, n_tiers = tr_variantListSize(tiers); i < n_tiers; ++i)
        {
            tr_variant* tier_list = tr_variantListChild(tiers, i);
            if (tier_list == nullptr)
            {
                continue;
            }

            for (size_t j = 0, jn = tr_variantListSize(tier_list); j < jn; ++j)
            {
                if (!tr_variantGetStrView(tr_variantListChild(tier_list, j), &url))
                {
                    continue;
                }

                setme.announce_list_.add(url, i);
            }
        }
    }

    // single 'announce' url
    if (std::empty(setme.announce_list_) && tr_variantDictFindStrView(meta, TR_KEY_announce, &url))
    {
        setme.announce_list_.add(url, 0);
    }

    return {};
}
#endif

static auto constexpr MaxBencDepth = 32;

struct MetainfoHandler final : public transmission::benc::BasicHandler<MaxBencDepth>
{
    using BasicHandler = transmission::benc::BasicHandler<MaxBencDepth>;

    tr_torrent_metainfo& tm_;
    int64_t piece_size_ = 0;
    int64_t length_ = 0;
    std::string encoding_ = "UTF-8";
    std::string_view info_dict_begin_;
    tr_tracker_tier_t tier_ = 0;
    // TODO: can we have a recycled std::string to avoid excess heap allocation
    std::vector<std::string> file_tree_;
    std::string_view pieces_root_;
    int64_t file_length_ = 0;

    enum class State
    {
        Top,
        Info,
        FileTree,
        Files,
        FilesIgnored,
    };
    State state_ = State::Top;

    explicit MetainfoHandler(tr_torrent_metainfo& tm)
        : tm_{ tm }
    {
    }

    bool Key(std::string_view key, Context const& context) override
    {
        BasicHandler::Key(key, context);
        std::cerr << __FILE__ << ':' << __LINE__ << " Key[" << key << "] depth[" << depth() << ']' << std::endl;
        return true;
    }

    bool StartDict(Context const& context) override
    {
        BasicHandler::StartDict(context);

        std::cerr << __FILE__ << ':' << __LINE__ << " in StartDict, depth is " << depth() << " and currentKey is "
                  << currentKey() << std::endl;

        if (state_ == State::FileTree)
        {
            auto const token = sanitizeToken(key(depth() - 1));
            if (!std::empty(token))
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " adding to file_tree_ [" << token << ']' << std::endl;
                file_tree_.emplace_back(token);
            }
        }
        else if (state_ == State::Top && depth() == 2 && key(1) == "info"sv)
        {
            info_dict_begin_ = context.raw();
            tm_.info_dict_offset_ = context.tokenSpan().first;
            state_ = State::Info;
        }
        else if (state_ == State::Info && key(depth() - 1) == "file tree"sv)
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " setting mode to 'file tree'" << std::endl;
            state_ = State::FileTree;
            file_tree_.clear();
            file_length_ = 0;
        }

        std::cerr << __FILE__ << ':' << __LINE__ << " start dict, depth " << depth() << std::endl;
        return true;
    }

    bool EndDict(Context const& context) override
    {
        BasicHandler::EndDict(context);
        std::cerr << __FILE__ << ':' << __LINE__ << " end dict, depth " << depth() << std::endl;

        if (state_ == State::Info && key(depth()) == "info"sv)
        {
            state_ = State::Top;
            return finishInfoDict(context);
        }

        if (state_ == State::FileTree) // bittorrent v2 format
        {
            if (!addFile(context))
            {
                return false;
            }

            auto const n = std::size(file_tree_);
            if (n > 0)
            {
                file_tree_.resize(n - 1);
            }

            if (key(depth()) == "file tree"sv)
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " changing mode from 'file tree' to 'none'" << std::endl;
                state_ = State::Info;
            }

            return true;
        }

        if (state_ == State::Files) // bittorrent v1 format
        {
            if (!addFile(context))
            {
                return false;
            }

            file_tree_.clear();
            return true;
        }

        return depth() > 0 || finish(context);
    }

    bool StartArray(Context const& context) override
    {
        BasicHandler::StartArray(context);
        std::cerr << __FILE__ << ':' << __LINE__ << " start array, depth " << depth() << std::endl;

        if (state_ == State::Info && key(depth() - 1) == "files"sv)
        {
            if (!std::empty(tm_.files_))
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " already have files from 'file list'; ignoring 'files'"
                          << std::endl;
                state_ = State::FilesIgnored;
            }
            else
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " setting mode to 'files'" << std::endl;
                state_ = State::Files;
                file_tree_.clear();
                file_length_ = 0;
            }
        }
        return true;
    }

    bool EndArray(Context const& context) override
    {
        BasicHandler::EndArray(context);
        std::cerr << __FILE__ << ':' << __LINE__ << " end array, depth " << depth() << std::endl;

        if ((state_ == State::Files || state_ == State::FilesIgnored) && key(depth()) == "files"sv) // bittorrent v1 format
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " changing mode from 'files' to 'none'" << std::endl;
            state_ = State::Info;
            return true;
        }

        if (depth() == 2 && key(1) == "announce-list")
        {
            ++tier_;
            std::cerr << __FILE__ << ':' << __LINE__ << " incrementing tier to " << tier_ << std::endl;
        }

        return true;
    }

    bool Int64(int64_t value, Context const& context) override
    {
        auto const curdepth = depth();
        auto const curkey = currentKey();
        auto unhandled = bool{ false };
        std::cerr << __FILE__ << ':' << __LINE__ << " Int[" << value << "] depth[" << depth() << "] currentKey[" << curkey
                  << ']' << std::endl;

        if (state_ == State::FilesIgnored)
        {
            // no-op
        }
        else if (curdepth == 1)
        {
            if (curkey == "creation date"sv)
            {
                tm_.date_created_ = value;
            }
            else if (curkey == "private"sv)
            {
                tm_.is_private_ = value != 0;
            }
            else if (curkey == "piece length"sv)
            {
                piece_size_ = value;
            }
            else
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                unhandled = true;
            }
        }
        else if (curdepth == 2 && key(1) == "info"sv)
        {
            if (curkey == "piece length"sv)
            {
                piece_size_ = value;
            }
            else if (curkey == "private"sv)
            {
                tm_.is_private_ = value != 0;
            }
            else if (curkey == "length"sv)
            {
                length_ = value;
            }
            else if (curkey == "meta version")
            {
                // currently unused. TODO support for bittorrent v2
                // TODO https://github.com/transmission/transmission/issues/458
            }
            else
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                unhandled = true;
            }
        }
        else if (state_ == State::FileTree || state_ == State::Files)
        {
            if (curkey == "length"sv)
            {
                file_length_ = value;
            }
            else
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                unhandled = true;
            }
        }
        else
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
            unhandled = true;
        }

        if (unhandled && !tr_error_is_set(context.error))
        {
            auto const errmsg = tr_strvJoin("unexpected: key["sv, curkey, "] int["sv, std::to_string(value), "]"sv);
            tr_error_set(context.error, EINVAL, errmsg);
        }

        return true;
    }

    bool String(std::string_view value, Context const& context) override
    {
        auto const curdepth = depth();
        auto const curkey = currentKey();
        auto unhandled = bool{ false };
        std::cerr << __FILE__ << ':' << __LINE__ << " String[" << value << "] depth[" << depth() << "] currentKey[" << curkey
                  << ']' << std::endl;

        if (state_ == State::FilesIgnored)
        {
            // no-op
        }
        else if (state_ == State::FileTree)
        {
            if (curkey == "attr"sv || curkey == "pieces root"sv)
            {
                // currently unused. TODO support for bittorrent v2
                // TODO https://github.com/transmission/transmission/issues/458
            }
            else
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                unhandled = true;
            }
        }
        else if (state_ == State::Files)
        {
            if (curdepth > 1 && key(curdepth - 1) == "path"sv)
            {
                auto const token = sanitizeToken(value);
                std::cerr << __FILE__ << ':' << __LINE__ << " in files mode, appending token [" << token << ']' << std::endl;
                file_tree_.emplace_back(token);
            }
            else if (curkey == "attr"sv)
            {
                // currently unused. TODO support for bittorrent v2
                // TODO https://github.com/transmission/transmission/issues/458
            }
            else
            {
                std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                unhandled = true;
            }
        }
        else
        {
            switch (curdepth)
            {
            case 1:
#if 0
                    if (curkey == "name"sv || curkey == "name.utf-8"sv)
                    {
                        tr_strvUtf8Clean(value, tm_.name_);
                        std::cerr << __FILE__ << ':' << __LINE__ << " got name [" << tm_.name_ << ']' << std::endl;
                    }
                    else
#endif
                if (curkey == "comment"sv || curkey == "comment.utf-8"sv)
                {
                    tr_strvUtf8Clean(value, tm_.comment_);
                    std::cerr << __FILE__ << ':' << __LINE__ << " got comment [" << tm_.comment_ << ']' << std::endl;
                }
                else if (curkey == "created by"sv || curkey == "created by.utf-8"sv)
                {
                    tr_strvUtf8Clean(value, tm_.creator_);
                    std::cerr << __FILE__ << ':' << __LINE__ << " got creator [" << tm_.creator_ << ']' << std::endl;
                }
                else if (curkey == "source"sv)
                {
                    tr_strvUtf8Clean(value, tm_.source_);
                    std::cerr << __FILE__ << ':' << __LINE__ << " got source [" << tm_.source_ << ']' << std::endl;
                }
                else if (curkey == "announce"sv)
                {
                    std::cerr << __FILE__ << ':' << __LINE__ << " adding announce url [" << value << "] to tier [" << tier_
                              << ']' << std::endl;
                    tm_.announceList().add(value, tier_);
                }
                else if (curkey == "encoding"sv)
                {
                    encoding_ = tr_strvStrip(value);
                }
                else
                {
                    std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                    unhandled = true;
                }
                break;

            case 2:
                if (key(1) == "info"sv && curkey == "source"sv)
                {
                    tr_strvUtf8Clean(value, tm_.source_);
                }
                else if (key(1) == "info"sv && curkey == "pieces"sv)
                {
                    auto const n = std::size(value) / sizeof(tr_sha1_digest_t);
                    tm_.pieces_.resize(n);
                    std::copy_n(std::data(value), std::size(value), reinterpret_cast<char*>(std::data(tm_.pieces_)));
                }
                else if (key(1) == "info"sv && (curkey == "name"sv || curkey == "name.utf-8"sv))
                {
                    tr_strvUtf8Clean(value, tm_.name_);
                    std::cerr << __FILE__ << ':' << __LINE__ << " got name [" << tm_.name_ << ']' << std::endl;
                    auto const token = sanitizeToken(tm_.name_);
                    if (!std::empty(token))
                    {
                        for (auto& file : tm_.files_)
                        {
                            file.setSubpath(tr_strvJoin(token, "/"sv, file.path()));
                        }
                    }
                }
                else if (key(1) == "piece layers"sv)
                {
                    // currently unused. TODO support for bittorrent v2
                    // TODO https://github.com/transmission/transmission/issues/458
                }
                else
                {
                    std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                    unhandled = true;
                }
                break;

            case 3:
                if (key(1) == "announce-list")
                {
                    std::cerr << __FILE__ << ':' << __LINE__ << " adding announce url [" << value << "] to tier [" << tier_
                              << ']' << std::endl;
                    tm_.announceList().add(value, tier_);
                }
                else
                {
                    std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                    unhandled = true;
                }
                break;

            default:
                std::cerr << __FILE__ << ':' << __LINE__ << " setting unhandled to true" << std::endl;
                unhandled = true;
                break;
            }
        }

        if (unhandled && !tr_error_is_set(context.error))
        {
            auto const errmsg = tr_strvJoin("unexpected: key["sv, curkey, "] str["sv, value, "]"sv);
            tr_error_set(context.error, EINVAL, errmsg);
        }

        return true;
    }

private:
    [[nodiscard]] bool addFile(Context const& context)
    {
        int ok = true;

        if (file_length_ == 0)
        {
            return ok;
        }

        // Check to see if we already added this file. This is a safeguard for
        // hybrid torrents with duplicate info between "file tree" and "files"
        if (auto const filename = buildPath(); std::empty(filename))
        {
            auto const errmsg = tr_strvJoin("invalid path ["sv, filename, "]"sv);
            tr_error_set(context.error, EINVAL, errmsg);
            ok = false;
        }
        else
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " adding file [" << filename << "][" << file_length_ << ']'
                      << std::endl;
            tm_.files_.emplace_back(filename, file_length_);
        }

        file_length_ = 0;
        pieces_root_ = {};
        // NB: let caller decide how to clear file_tree_.
        // if we're in "files" mode we clear it; if in "file tree" we pop it
        return ok;
    }

    [[nodiscard]] std::string buildPath() const
    {
        auto path = std::string{};
        for (auto const& token : file_tree_)
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " token [" << token << ']' << std::endl;
            path += token;
            if (!std::empty(token))
            {
                path += '/';
            }
        }
        auto const n = std::size(path);
        if (n > 0)
        {
            path.resize(n - 1);
        }
        return path;
    }

    bool finishInfoDict(Context const& context)
    {
        if (std::empty(info_dict_begin_))
        {
            tr_error_set(context.error, EINVAL, "no info_dict found");
            return false;
        }

        TR_ASSERT(info_dict_begin_[0] == 'd');
        TR_ASSERT(context.raw().back() == 'e');
        char const* const begin = &info_dict_begin_.front();
        char const* const end = &context.raw().back() + 1;
        auto const info_dict_benc = std::string_view{ begin, size_t(end - begin) };
        auto const hash = tr_sha1(info_dict_benc);
        if (!hash)
        {
            tr_error_set(context.error, EINVAL, "bad info_dict checksum");
        }
        tm_.info_hash_ = *hash;
        tm_.info_hash_str_ = tr_sha1_to_string(tm_.info_hash_);
        tm_.info_dict_size_ = std::size(info_dict_benc);
        std::cerr << __FILE__ << ':' << __LINE__ << " info_dict_size " << tm_.info_dict_size_ << std::endl;
        std::cerr << __FILE__ << ':' << __LINE__ << " info_dict_offset " << tm_.info_dict_offset_ << std::endl;

        // In addition, remember the offset of the pieces dictionary entry.
        // This will be useful when we load piece checksums on demand.
        auto constexpr Key = "6:pieces"sv;
        auto const pit = std::search(std::begin(info_dict_benc), std::end(info_dict_benc), std::begin(Key), std::end(Key));
        tm_.pieces_offset_ = tm_.info_dict_offset_ + std::distance(std::begin(info_dict_benc), pit) + std::size(Key);
        std::cerr << __FILE__ << ':' << __LINE__ << " pieces_offset_ " << tm_.pieces_offset_ << std::endl;

        return true;
    }

    bool finish(Context const& context)
    {
        std::cerr << __FILE__ << ':' << __LINE__ << std::endl;

        // bittorrent 1.0 spec
        // http://bittorrent.org/beps/bep_0003.html
        //
        // "There is also a key length or a key files, but not both or neither.
        //
        // "If length is present then the download represents a single file,
        // otherwise it represents a set of files which go in a directory structure.
        // In the single file case, length maps to the length of the file in bytes.
        if (tm_.fileCount() == 0 && length_ != 0 && !std::empty(tm_.name_))
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " single file found" << std::endl;
            tm_.files_.emplace_back(tm_.name_, length_);
        }

        if (tm_.fileCount() == 0)
        {
            std::cerr << __FILE__ << ':' << __LINE__ << std::endl;
            if (!tr_error_is_set(context.error))
            {
                tr_error_set(context.error, EINVAL, "no files found");
            }
            return false;
        }

        std::cerr << __FILE__ << ':' << __LINE__ << std::endl;
        if (piece_size_ == 0)
        {
            std::cerr << __FILE__ << ':' << __LINE__ << std::endl;
            auto const errmsg = tr_strvJoin("invalid piece size: ", std::to_string(piece_size_));
            if (!tr_error_is_set(context.error))
            {
                tr_error_set(context.error, EINVAL, errmsg);
            }
            return false;
        }

        std::cerr << __FILE__ << ':' << __LINE__ << " walking files " << std::endl;
        for (auto const& file : tm_.files_)
        {
            std::cerr << __FILE__ << ':' << __LINE__ << " filename [" << file.path() << "] size[" << file.size() << ']'
                      << std::endl;
        }
        auto const total_size = std::accumulate(
            std::begin(tm_.files_),
            std::end(tm_.files_),
            uint64_t{ 0 },
            [](auto const sum, auto const& file) { return sum + file.size(); });
        std::cerr << __FILE__ << ':' << __LINE__ << " total_size[" << total_size << "] piece_size_[" << piece_size_ << ']'
                  << std::endl;
        tm_.block_info_.initSizes(total_size, piece_size_);
        return true;
    }
};

#if 0
std::string_view tr_torrent_metainfo::parseImpl(tr_torrent_metainfo& setme, tr_variant* meta, std::string_view benc)
{
    int64_t i = 0;
    auto sv = std::string_view{};

    // info_hash: urlencoded 20-byte SHA1 hash of the value of the info key
    // from the Metainfo file. Note that the value will be a bencoded
    // dictionary, given the definition of the info key above.
    tr_variant* info_dict = nullptr;
    if (tr_variantDictFindDict(meta, TR_KEY_info, &info_dict))
    {
        // Calculate the hash of the `info` dict.
        // This is the torrent's unique ID and is central to everything.
        auto const info_dict_benc = tr_variantToStr(info_dict, TR_VARIANT_FMT_BENC);
        auto const hash = tr_sha1(info_dict_benc);
        if (!hash)
        {
            return "bad info_dict checksum";
        }
        setme.info_hash_ = *hash;
        setme.info_hash_str_ = tr_sha1_to_string(setme.info_hash_);

        // Remember the offset and length of the bencoded info dict.
        // This is important when providing metainfo to magnet peers
        // (see http://bittorrent.org/beps/bep_0009.html for details).
        //
        // Calculating this later from scratch is kind of expensive,
        // so do it here since we've already got the bencoded info dict.
        auto const it = std::search(std::begin(benc), std::end(benc), std::begin(info_dict_benc), std::end(info_dict_benc));
        setme.info_dict_offset_ = std::distance(std::begin(benc), it);
        setme.info_dict_size_ = std::size(info_dict_benc);

        // In addition, remember the offset of the pieces dictionary entry.
        // This will be useful when we load piece checksums on demand.
        auto constexpr Key = "6:pieces"sv;
        auto const pit = std::search(std::begin(benc), std::end(benc), std::begin(Key), std::end(Key));
        setme.pieces_offset_ = std::distance(std::begin(benc), pit) + std::size(Key);
    }
    else
    {
        return "missing 'info' dictionary";
    }


    // pieces
    if (!tr_variantDictFindStrView(info_dict, TR_KEY_pieces, &sv) || (std::size(sv) % sizeof(tr_sha1_digest_t) != 0))
    {
        return "'info' dict 'pieces' is missing or has an invalid value";
    }
    auto const n = std::size(sv) / sizeof(tr_sha1_digest_t);
    setme.pieces_.resize(n);
    std::copy_n(std::data(sv), std::size(sv), reinterpret_cast<char*>(std::data(setme.pieces_)));

    // files
    auto total_size = uint64_t{ 0 };
    if (auto const errstr = parseFiles(setme, info_dict, &total_size); !std::empty(errstr))
    {
        return errstr;
    }

    if (std::empty(setme.files_))
    {
        return "no files found"sv;
    }

    // do the size and piece size match up?
    setme.block_info_.initSizes(total_size, piece_size);
    if (setme.block_info_.n_pieces != std::size(setme.pieces_))
    {
        return "piece count and file sizes do not match";
    }

    parseAnnounce(setme, meta);
    parseWebseeds(setme, meta);

    return {};
}
#endif

bool tr_torrent_metainfo::parseBenc(std::string_view benc, tr_error** error)
{
    auto stack = transmission::benc::ParserStack<MaxBencDepth>{};
    auto handler = MetainfoHandler{ *this };

    tr_error* my_error = nullptr;

    if (error == nullptr)
    {
        error = &my_error;
    }
    auto const ok = transmission::benc::parse(benc, stack, handler, nullptr, error);
    std::cerr << __FILE__ << ':' << __LINE__ << " parseBenc error is set: " << tr_error_is_set(error) << std::endl;

    if (tr_error_is_set(error))
    {
        std::cerr << __FILE__ << ':' << __LINE__ << " (*error)->message " << (*error)->message;
        tr_logAddError("%s", (*error)->message);
    }

    if (!ok)
    {
        return false;
    }

    if (std::empty(name_))
    {
        // TODO from first file
    }

    return true;
}

bool tr_torrent_metainfo::parseTorrentFile(std::string_view filename, std::vector<char>* contents, tr_error** error)
{
    auto local_contents = std::vector<char>{};

    if (contents == nullptr)
    {
        contents = &local_contents;
    }

    auto const sz_filename = std::string{ filename };
    return tr_loadFile(*contents, sz_filename, error) && parseBenc({ std::data(*contents), std::size(*contents) }, error);
}

tr_sha1_digest_t const& tr_torrent_metainfo::pieceHash(tr_piece_index_t piece) const
{
    return this->pieces_[piece];
}

std::string tr_torrent_metainfo::makeFilename(
    std::string_view dirname,
    std::string_view name,
    std::string_view info_hash_string,
    BasenameFormat format,
    std::string_view suffix)
{
    // `${dirname}/${name}.${info_hash}${suffix}`
    // `${dirname}/${info_hash}${suffix}`
    return format == BasenameFormat::Hash ? tr_strvJoin(dirname, "/"sv, info_hash_string, suffix) :
                                            tr_strvJoin(dirname, "/"sv, name, "."sv, info_hash_string.substr(0, 16), suffix);
}

bool tr_torrent_metainfo::migrateFile(
    std::string_view dirname,
    std::string_view name,
    std::string_view info_hash_string,
    std::string_view suffix)
{
    auto const old_filename = makeFilename(dirname, name, info_hash_string, BasenameFormat::NameAndPartialHash, suffix);
    auto const old_filename_exists = tr_sys_path_exists(old_filename.c_str(), nullptr);
    auto const new_filename = makeFilename(dirname, name, info_hash_string, BasenameFormat::Hash, suffix);
    auto const new_filename_exists = tr_sys_path_exists(new_filename.c_str(), nullptr);

    if (old_filename_exists && new_filename_exists)
    {
        tr_sys_path_remove(old_filename.c_str(), nullptr);
        return false;
    }

    if (new_filename_exists)
    {
        return false;
    }

    if (old_filename_exists && tr_sys_path_rename(old_filename.c_str(), new_filename.c_str(), nullptr))
    {
        auto const name_sz = std::string{ name };
        tr_logAddNamedError(
            name_sz.c_str(),
            "Migrated torrent file from \"%s\" to \"%s\"",
            old_filename.c_str(),
            new_filename.c_str());
        return true;
    }

    return false; // neither file exists
}

void tr_torrent_metainfo::removeFile(
    std::string_view dirname,
    std::string_view name,
    std::string_view info_hash_string,
    std::string_view suffix)
{
    auto filename = makeFilename(dirname, name, info_hash_string, BasenameFormat::NameAndPartialHash, suffix);
    tr_sys_path_remove(filename.c_str(), nullptr);

    filename = makeFilename(dirname, name, info_hash_string, BasenameFormat::Hash, suffix);
    tr_sys_path_remove(filename.c_str(), nullptr);
}

std::string const& tr_torrent_metainfo::fileSubpath(tr_file_index_t i) const
{
    TR_ASSERT(i < fileCount());

    return files_.at(i).path();
}

void tr_torrent_metainfo::setFileSubpath(tr_file_index_t i, std::string_view subpath)
{
    TR_ASSERT(i < fileCount());

    files_.at(i).setSubpath(subpath);
}

uint64_t tr_torrent_metainfo::fileSize(tr_file_index_t i) const
{
    TR_ASSERT(i < fileCount());

    return files_.at(i).size();
}
