// SPDX-License-Identifier: MIT
// Copyright 2026 kubuntu-customization contributors
#include "search.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QMimeDatabase>
#include <QJsonArray>
#include <QDateTime>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QStringDecoder>
#include <cmath>
#include <limits>
#include <linux/stat.h>
#include <sys/syscall.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace {
enum class Field { Name, Kind, Size, Modified, Created, Tag, Content };
enum class Op { Contains, Equals, Begins, Ends, AtLeast, AtMost, Greater, Less };
struct Criterion {
    QString match;
    std::vector<Criterion> children;
    Field field = Field::Name;
    Op op = Op::Contains;
    QString value;
    qint64 number = 0;
};

bool integerValue(const QJsonValue &value, qint64 &number) {
    if (value.isString()) {
        const auto text = value.toString();
        if (text.isEmpty() || text.size() > 19 ||
            std::any_of(text.begin(), text.end(), [](QChar c) { return c < '0' || c > '9'; })) return false;
        bool ok = false;
        number = text.toLongLong(&ok);
        return ok && number >= 0;
    }
    if (!value.isDouble()) return false;
    const double n = value.toDouble();
    if (!std::isfinite(n) || n < 0 || n > 9007199254740991.0 || std::floor(n) != n) return false;
    number = static_cast<qint64>(n);
    return true;
}

bool compile(const QJsonObject &json, Criterion &node, QString &error, int &nodes, int depth = 0) {
    auto reject = [&](const QString &message) { error = message; return false; };
    if (++nodes > 128 || depth > 12) return reject(QStringLiteral("Search criteria exceed 128 nodes or 12 levels."));
    if (json.contains("match")) {
        if (json.size() != 2 || !json.value("criteria").isArray())
            return reject(QStringLiteral("A criteria group requires match and a criteria array."));
        node.match = json.value("match").toString();
        if (node.match != "all" && node.match != "any" && node.match != "none")
            return reject(QStringLiteral("Criteria groups must match all, any or none."));
        const auto children = json.value("criteria").toArray();
        if (children.isEmpty() || children.size() > 128)
            return reject(QStringLiteral("A criteria group needs 1 to 128 conditions."));
        for (const auto &child : children) {
            Criterion next;
            if (!child.isObject() || !compile(child.toObject(), next, error, nodes, depth + 1)) {
                if (error.isEmpty()) error = QStringLiteral("Each search condition must be an object.");
                return false;
            }
            node.children.push_back(std::move(next));
        }
        return true;
    }
    if (json.size() != 3 || !json.contains("field") || !json.contains("op") || !json.contains("value"))
        return reject(QStringLiteral("A search condition requires field, op and value."));
    static const QMap<QString, Field> fields{{"name", Field::Name}, {"kind", Field::Kind}, {"size", Field::Size},
        {"modified", Field::Modified}, {"created", Field::Created}, {"tag", Field::Tag}, {"content", Field::Content}};
    static const QMap<QString, Op> ops{{"contains", Op::Contains}, {"equals", Op::Equals}, {"beginsWith", Op::Begins},
        {"endsWith", Op::Ends}, {"atLeast", Op::AtLeast}, {"atMost", Op::AtMost}, {"greaterThan", Op::Greater}, {"lessThan", Op::Less}};
    const auto field = fields.constFind(json.value("field").toString());
    const auto op = ops.constFind(json.value("op").toString());
    if (field == fields.cend() || op == ops.cend()) return reject(QStringLiteral("Unknown search field or operator."));
    node.field = *field;
    node.op = *op;
    const bool numeric = node.field == Field::Size || node.field == Field::Modified || node.field == Field::Created;
    if (numeric) {
        if (node.op == Op::Contains || node.op == Op::Begins || node.op == Op::Ends || !integerValue(json.value("value"), node.number))
            return reject(QStringLiteral("Size and date conditions require a nonnegative exact integer and a numeric comparison."));
    } else {
        if (!json.value("value").isString()) return reject(QStringLiteral("Text conditions require a string."));
        node.value = json.value("value").toString();
        if (node.value.trimmed().isEmpty() || node.value.size() > 4096 || node.value.contains(QChar::Null))
            return reject(QStringLiteral("Text conditions require 1 to 4096 characters without NUL."));
        if (node.op != Op::Contains && node.op != Op::Equals && node.op != Op::Begins && node.op != Op::Ends)
            return reject(QStringLiteral("Text fields require a text comparison."));
        if ((node.field == Field::Kind || node.field == Field::Tag) && node.op != Op::Equals)
            return reject(QStringLiteral("Kind and tag conditions require equals."));
        if (node.field == Field::Kind) {
            node.value = node.value.toLower();
            static const QStringList kinds{"file", "folder", "image", "pdf", "text", "audio", "video", "archive", "application", "link"};
            if (!kinds.contains(node.value) && !QRegularExpression(QStringLiteral("^[a-z0-9.+-]+/[a-z0-9.+-]+$")).match(node.value).hasMatch())
                return reject(QStringLiteral("Unknown file kind; use a supported kind or MIME type."));
        }
    }
    return true;
}

QJsonObject group(const QString &match, const QJsonArray &children) {
    return {{"match", match}, {"criteria", children}};
}
QJsonObject leaf(const QString &field, const QString &op, const QJsonValue &value) {
    return {{"field", field}, {"op", op}, {"value", value}};
}

struct Token { QString text; bool quoted = false; };
class QueryParser {
public:
    QString error;
    QJsonObject parse(const QString &query) {
        if (query.size() > 16384 || query.contains(QChar::Null)) { error = "Query exceeds 16384 characters or contains NUL."; return {}; }
        for (qsizetype i = 0; i < query.size();) {
            if (query[i].isSpace()) { ++i; continue; }
            Token token;
            if (query[i] == '"' || query[i] == '\'') {
                token.quoted = true;
                const auto quote = query[i++];
                bool closed = false;
                while (i < query.size()) {
                    const auto c = query[i++];
                    if (c == quote) { closed = true; break; }
                    if (c == '\\' && i < query.size()) token.text += query[i++];
                    else token.text += c;
                }
                if (!closed) { error = "Unclosed quoted search text."; return {}; }
            } else if (QStringLiteral("():=<>!").contains(query[i])) {
                token.text = query[i++];
                if (i < query.size() && query[i] == '=' && (token.text == ">" || token.text == "<" || token.text == "!")) token.text += query[i++];
            } else {
                while (i < query.size() && !query[i].isSpace() && !QStringLiteral("():=<>!\"'").contains(query[i])) token.text += query[i++];
            }
            tokens.append(token);
            if (tokens.size() > 512) { error = "Query exceeds 512 tokens."; return {}; }
        }
        if (tokens.isEmpty()) return {};
        auto result = expression(0);
        if (error.isEmpty() && position != tokens.size()) error = "Unexpected query token or closing parenthesis.";
        Criterion compiled;
        int nodes = 0;
        if (error.isEmpty()) compile(result, compiled, error, nodes);
        return error.isEmpty() ? result : QJsonObject{};
    }
private:
    QList<Token> tokens;
    qsizetype position = 0;
    bool next(const QString &text) const { return position < tokens.size() && !tokens[position].quoted && tokens[position].text == text; }
    bool take(const QString &text) { if (!next(text)) return false; ++position; return true; }
    QJsonObject expression(int depth) {
        QJsonArray parts{conjunction(depth)};
        while (error.isEmpty() && take("OR")) parts.append(conjunction(depth));
        return parts.size() == 1 ? parts.first().toObject() : group("any", parts);
    }
    QJsonObject conjunction(int depth) {
        QJsonArray parts{unary(depth)};
        while (error.isEmpty() && position < tokens.size() && !next(")") && !next("OR")) {
            take("AND");
            parts.append(unary(depth));
        }
        return parts.size() == 1 ? parts.first().toObject() : group("all", parts);
    }
    QJsonObject unary(int depth) {
        if (depth > 12) { error = "Query exceeds 12 nesting levels."; return {}; }
        if (take("NOT")) return group("none", {unary(depth + 1)});
        if (take("(")) {
            auto result = expression(depth + 1);
            if (!take(")")) error = "Missing closing query parenthesis.";
            return result;
        }
        if (position >= tokens.size() || next(")") || next("AND") || next("OR")) { error = "Expected a search condition."; return {}; }
        const auto token = tokens[position++];
        QString field = "name", op = "contains";
        QString value = token.text;
        if (!token.quoted && (next(":") || next("=") || next(">") || next("<") || next(">=") || next("<=") || next("!="))) {
            field = token.text.toLower();
            if (field == "filename") field = "name";
            if (field == "date") field = "modified";
            const auto comparison = tokens[position++].text;
            if (position >= tokens.size() || next(")") || next("(")) { error = "Missing keyword value."; return {}; }
            value = tokens[position++].text;
            bool negate = comparison == "!=";
            op = comparison == ">" ? "greaterThan" : comparison == "<" ? "lessThan" : comparison == ">=" ? "atLeast" : comparison == "<=" ? "atMost" : comparison == ":" ? "contains" : "equals";
            if ((field == "kind" || field == "tag" || field == "size" || field == "modified" || field == "created") && comparison == ":") op = "equals";
            QJsonValue jsonValue(value);
            if (field == "size") {
                const auto match = QRegularExpression(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)(B|KB|MB|GB|TB|KIB|MIB|GIB|TIB)?$"), QRegularExpression::CaseInsensitiveOption).match(value);
                if (!match.hasMatch()) { error = "Size requires bytes or B/KB/MB/GB/TB/KiB/MiB/GiB/TiB units."; return {}; }
                const auto unit = match.captured(2).toUpper();
                if (unit.isEmpty() || unit == "B") {
                    qint64 n;
                    if (!integerValue(match.captured(1), n)) { error = "Size requires an exact integer byte count."; return {}; }
                    jsonValue = QString::number(n);
                } else {
                    const int power = QStringLiteral("KMGT").indexOf(unit[0]) + 1;
                    const long double bytes = match.captured(1).toDouble() * std::pow(unit.contains('I') ? 1024.0L : 1000.0L, power);
                    if (bytes < 0 || bytes > std::numeric_limits<qint64>::max() || std::floor(bytes) != bytes) { error = "Size is outside the supported byte range."; return {}; }
                    jsonValue = QString::number(static_cast<qint64>(bytes));
                }
            } else if (field == "modified" || field == "created") {
                qint64 seconds;
                if (integerValue(value, seconds)) jsonValue = QString::number(seconds);
                else {
                    auto date = QDate::fromString(value, Qt::ISODate);
                    auto datetime = QDateTime::fromString(value, Qt::ISODate);
                    if (date.isValid()) datetime = date.startOfDay();
                    if (!datetime.isValid() || datetime.toSecsSinceEpoch() < 0) { error = "Date requires ISO YYYY-MM-DD, a quoted ISO datetime, or Unix seconds."; return {}; }
                    if (date.isValid()) {
                        const auto begin = datetime.toSecsSinceEpoch();
                        const auto end = date.addDays(1).startOfDay().toSecsSinceEpoch();
                        if (op == "equals") {
                            auto day = group("all", {leaf(field, "atLeast", QString::number(begin)), leaf(field, "lessThan", QString::number(end))});
                            return negate ? group("none", {day}) : day;
                        }
                        seconds = (op == "greaterThan" || op == "atMost") ? end - 1 : begin;
                    } else seconds = datetime.toSecsSinceEpoch();
                    jsonValue = QString::number(seconds);
                }
            }
            auto result = leaf(field, op, jsonValue);
            return negate ? group("none", {result}) : result;
        }
        return leaf(field, op, value);
    }
};

struct Request {
    quint64 generation;
    QUrl root;
    QString query;
    LocalFileSearch::Options options;
    std::shared_ptr<std::atomic_bool> cancel;
};

struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::shared_ptr<std::atomic_bool> active;
    bool stopping = false;
};

struct Mount {
    QByteArray path;
    bool local;
};

QByteArray mountPath(QByteArray path) {
    path.replace("\\040", " ");
    path.replace("\\011", "\t");
    path.replace("\\012", "\n");
    path.replace("\\134", "\\");
    return path;
}

// Inspect mount metadata before opening a mount. QStorageInfo::refresh/statfs
// can itself block on an unavailable network filesystem, so it is not used.
bool loadMounts(QList<Mount> &mounts) {
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto data = file.read(4 * 1024 * 1024);
    const QList<QByteArray> localTypes{"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "zfs",
        "tmpfs", "ramfs", "overlay", "ubifs", "ufs", "jfs", "reiserfs", "ntfs", "ntfs3",
        "vfat", "exfat", "iso9660", "udf", "squashfs", "fuseblk", "bcachefs"};
    for (const auto &line : data.split('\n')) {
        const auto separator = line.indexOf(" - ");
        if (separator < 0) continue;
        const auto fields = line.left(separator).split(' ');
        const auto trailing = line.mid(separator + 3).split(' ');
        if (fields.size() < 6 || trailing.isEmpty()) continue;
        mounts.append({mountPath(fields[4]), localTypes.contains(trailing[0])});
    }
    std::stable_sort(mounts.begin(), mounts.end(), [](const Mount &a, const Mount &b) {
        return a.path.size() > b.path.size();
    });
    return std::any_of(mounts.cbegin(), mounts.cend(), [](const Mount &mount) { return mount.path == "/"; });
}

bool localPath(const QByteArray &path, const QList<Mount> &mounts) {
    for (const auto &mount : mounts) {
        if (path == mount.path || (mount.path == "/") || path.startsWith(mount.path + '/'))
            return mount.local;
    }
    return false;
}

struct DirectoryCloser {
    void operator()(DIR *directory) const { if (directory) closedir(directory); }
};
using Directory = std::unique_ptr<DIR, DirectoryCloser>;
struct Frame {
    QByteArray path;
    Directory directory;
};

// Walk components relative to already-open directory descriptors. O_NOFOLLOW
// prevents a directory (including an ancestor of the root) being replaced with
// a symlink that leads outside the selected tree or onto a network mount.
int openRoot(const QByteArray &path, const QList<Mount> &mounts,
             const std::shared_ptr<std::atomic_bool> &cancel) {
    if (!localPath("/", mounts)) { errno = EXDEV; return -1; }
    int descriptor = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return -1;
    QByteArray current;
    for (const auto &part : path.split('/')) {
        if (part.isEmpty()) continue;
        if (cancel->load(std::memory_order_relaxed)) { close(descriptor); errno = ECANCELED; return -1; }
        current += '/' + part;
        if (!localPath(current, mounts)) { close(descriptor); errno = EXDEV; return -1; }
        const int next = openat(descriptor, part.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        const int failure = errno;
        close(descriptor);
        if (next < 0) { errno = failure; return -1; }
        descriptor = next;
    }
    return descriptor;
}

QString readableError(int code) {
    if (code == EXDEV) return QStringLiteral("Recursive search is available on local folders. Network and virtual mounts are skipped.");
    if (code == ELOOP || code == ENOTDIR) return QStringLiteral("Choose a folder without symbolic links in its path for recursive search.");
    if (code == EACCES || code == EPERM) return QStringLiteral("The search folder is not readable.");
    if (code == ENOENT) return QStringLiteral("The search folder no longer exists.");
    return QStringLiteral("Could not read the search folder: %1").arg(QString::fromLocal8Bit(std::strerror(code)));
}

struct Descriptor {
    int fd = -1;
    explicit Descriptor(int fd = -1) : fd(fd) {}
    ~Descriptor() { if (fd >= 0) close(fd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};
enum class Truth { No, Yes, Unknown };
Truth truth(bool value) { return value ? Truth::Yes : Truth::No; }
bool textMatch(const QString &text, const Criterion &node) {
    if (node.op == Op::Equals) return text.compare(node.value, Qt::CaseInsensitive) == 0;
    if (node.op == Op::Begins) return text.startsWith(node.value, Qt::CaseInsensitive);
    if (node.op == Op::Ends) return text.endsWith(node.value, Qt::CaseInsensitive);
    return text.contains(node.value, Qt::CaseInsensitive);
}
bool numberMatch(qint64 number, const Criterion &node) {
    if (node.op == Op::AtLeast) return number >= node.number;
    if (node.op == Op::AtMost) return number <= node.number;
    if (node.op == Op::Greater) return number > node.number;
    if (node.op == Op::Less) return number < node.number;
    return number == node.number;
}
struct Metrics {
    qint64 contentBytes = 0;
    int incompleteContent = 0;
    int unavailableMetadata = 0;
};
struct Entry {
    QString name;
    Descriptor pinned;
    struct stat metadata{};
    bool valid = false;
    bool tagsLoaded = false, tagsKnown = false;
    QStringList tags;
    bool birthLoaded = false;
    std::optional<qint64> birth;
    bool contentLoaded = false, contentComplete = false, contentKnown = false;
    QString content;
    QMimeDatabase &mimes;
    const Request &request;
    QElapsedTimer &elapsed;
    Metrics &metrics;
    const QHash<QString, QSet<QByteArray>> *indexedContent = nullptr;
    QByteArray path;

    Entry(int parent, const QByteArray &filename, QMimeDatabase &mimes, const Request &request,
          QElapsedTimer &elapsed, Metrics &metrics) : name(QFile::decodeName(filename)),
        pinned(openat(parent, filename.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC)),
        mimes(mimes), request(request), elapsed(elapsed), metrics(metrics) {
        valid = pinned.fd >= 0 && fstat(pinned.fd, &metadata) == 0;
        if (!valid) ++metrics.unavailableMetadata;
    }
    bool stopped() const {
        return request.cancel->load(std::memory_order_relaxed) || elapsed.elapsed() >= request.options.maxElapsedMs;
    }
    QString mime() { return mimes.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name(); }
    bool kind(const QString &value) {
        if (value == "folder") return S_ISDIR(metadata.st_mode);
        if (value == "file") return S_ISREG(metadata.st_mode);
        if (value == "link") return S_ISLNK(metadata.st_mode);
        if (!S_ISREG(metadata.st_mode)) return false;
        const auto type = mimes.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
        if (value.contains('/')) return type.inherits(value);
        if (value == "image" || value == "audio" || value == "video") return type.name().startsWith(value + '/');
        if (value == "pdf") return type.inherits("application/pdf");
        if (value == "text") return type.inherits("text/plain");
        if (value == "application") return type.inherits("application/x-executable") || type.inherits("application/x-desktop") || type.inherits("application/x-sharedlib");
        if (value == "archive") return type.inherits("application/zip") || type.inherits("application/x-tar") || type.inherits("application/x-compressed-tar") ||
            type.inherits("application/x-7z-compressed") || type.inherits("application/vnd.rar") || type.inherits("application/gzip") || type.inherits("application/x-bzip2") || type.inherits("application/x-xz");
        return false;
    }
    Truth tagged(const QString &value) {
        if (!tagsLoaded) {
            tagsLoaded = true;
            if (S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode)) {
                char data[65536];
                const auto path = "/proc/self/fd/" + QByteArray::number(pinned.fd);
                const auto length = getxattr(path.constData(), "user.xdg.tags", data, sizeof(data));
                if (length >= 0) {
                    tagsKnown = true;
                    tags = QString::fromUtf8(data, length).split(',', Qt::SkipEmptyParts);
                    for (auto &tag : tags) tag = tag.trimmed();
                } else if (errno == ENODATA || errno == ENOTSUP) tagsKnown = true;
            }
            if (!tagsKnown) ++metrics.unavailableMetadata;
        }
        return tagsKnown ? truth(tags.contains(value, Qt::CaseInsensitive)) : Truth::Unknown;
    }
    Truth created(const Criterion &node) {
        if (!birthLoaded) {
            birthLoaded = true;
#ifdef SYS_statx
            struct statx status{};
            if (syscall(SYS_statx, pinned.fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_BTIME, &status) == 0 && (status.stx_mask & STATX_BTIME))
                birth = status.stx_btime.tv_sec;
#endif
            if (!birth) ++metrics.unavailableMetadata;
        }
        return birth ? truth(numberMatch(*birth, node)) : Truth::Unknown;
    }
    void loadContent() {
        contentLoaded = true;
        if (!S_ISREG(metadata.st_mode)) { ++metrics.incompleteContent; return; }
        const qint64 allowance = std::min<qint64>(request.options.maxContentBytes,
            request.options.maxTotalContentBytes - metrics.contentBytes);
        if (allowance <= 0 || stopped()) { ++metrics.incompleteContent; return; }
        // Reopen the pinned inode, not its potentially changed pathname.
        const auto path = "/proc/self/fd/" + QByteArray::number(pinned.fd);
        Descriptor input(open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        struct stat before{};
        if (input.fd < 0 || fstat(input.fd, &before) != 0 || !S_ISREG(before.st_mode) ||
            before.st_ino != metadata.st_ino || before.st_dev != metadata.st_dev) { ++metrics.incompleteContent; return; }
        QByteArray bytes;
        bool readError = false;
        while (bytes.size() < allowance && !stopped()) {
            char data[16384];
            const auto length = read(input.fd, data, std::min<qint64>(sizeof(data), allowance - bytes.size()));
            if (length < 0) { if (errno == EINTR) continue; readError = true; break; }
            if (length == 0) break;
            bytes.append(data, length);
            metrics.contentBytes += length;
        }
        struct stat after{};
        const bool stable = fstat(input.fd, &after) == 0 && before.st_size == after.st_size &&
            before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
            before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
        if (!stable || stopped() || readError) { ++metrics.incompleteContent; return; }
        contentComplete = bytes.size() >= before.st_size;
        const auto encoding = QStringConverter::encodingForData(bytes).value_or(QStringConverter::Utf8);
        const bool unicodeWide = encoding == QStringConverter::Utf16LE || encoding == QStringConverter::Utf16BE ||
            encoding == QStringConverter::Utf32LE || encoding == QStringConverter::Utf32BE;
        if (!unicodeWide && bytes.contains('\0')) { ++metrics.incompleteContent; return; }
        QStringDecoder decoder(encoding);
        content = decoder.decode(bytes);
        contentKnown = !decoder.hasError();
        if (!contentComplete || !contentKnown) ++metrics.incompleteContent;
    }
    Truth contentMatch(const Criterion &node) {
        if (indexedContent && node.op == Op::Contains) {
            const auto found = indexedContent->constFind(node.value);
            if (found != indexedContent->cend() && found->contains(path)) return Truth::Yes;
        }
        if (!contentLoaded) loadContent();
        if (!contentKnown) return Truth::Unknown;
        const bool matches = textMatch(content, node);
        // A prefix can prove a positive contains/beginsWith, but cannot prove
        // absence, equality or suffix until the entire file has been read.
        if (!contentComplete && (node.op == Op::Equals || node.op == Op::Ends || !matches)) return Truth::Unknown;
        return truth(matches);
    }
    Truth evaluate(const Criterion &node) {
        if (stopped()) return Truth::Unknown;
        if (!node.match.isEmpty()) {
            bool unknown = false;
            for (const auto &child : node.children) {
                const auto result = evaluate(child);
                if (node.match == "all" && result == Truth::No) return Truth::No;
                if (node.match == "any" && result == Truth::Yes) return Truth::Yes;
                if (node.match == "none" && result == Truth::Yes) return Truth::No;
                unknown |= result == Truth::Unknown;
            }
            return unknown ? Truth::Unknown : node.match == "any" ? Truth::No : Truth::Yes;
        }
        if (node.field == Field::Name) return truth(textMatch(name, node));
        if (!valid) return Truth::Unknown;
        if (node.field == Field::Kind) return truth(kind(node.value));
        if (node.field == Field::Size) return numberMatch(metadata.st_size, node) ? Truth::Yes : Truth::No;
        if (node.field == Field::Modified) return truth(numberMatch(metadata.st_mtime, node));
        if (node.field == Field::Created) return created(node);
        if (node.field == Field::Tag) return tagged(node.value);
        return contentMatch(node);
    }
};

QJsonObject combinedCriteria(const Request &request, QString &error) {
    QJsonArray all;
    if (!request.query.isEmpty()) {
        if (request.options.parseKeywords) {
            const auto query = LocalFileSearch::parseQuery(request.query, &error);
            if (!error.isEmpty()) return {};
            if (!query.isEmpty()) all.append(query);
        } else all.append(leaf("name", "contains", request.query));
    }
    const auto &options = request.options;
    if (!options.criteria.isEmpty()) all.append(options.criteria);
    if (!options.kind.isEmpty()) all.append(leaf("kind", "equals", options.kind));
    if (!options.requiredTag.isEmpty()) all.append(leaf("tag", "equals", options.requiredTag));
    if (!options.contentText.trimmed().isEmpty()) all.append(leaf("content", "contains", options.contentText.trimmed()));
    if (options.minimumSize >= 0) all.append(leaf("size", "atLeast", QString::number(options.minimumSize)));
    if (options.maximumSize >= 0) all.append(leaf("size", "atMost", QString::number(options.maximumSize)));
    if (options.modifiedAfter >= 0) all.append(leaf("modified", "atLeast", QString::number(options.modifiedAfter)));
    if (options.modifiedBefore >= 0) all.append(leaf("modified", "atMost", QString::number(options.modifiedBefore)));
    return all.isEmpty() ? QJsonObject{} : all.size() == 1 ? all.first().toObject() : group("all", all);
}

// Public Baloo 6.24 query CLI and implementation reviewed at KDE/baloo
// v6.24.0: tools/baloosearch/main.cpp, lib/searchstore.cpp, engine/global.cpp.
// --sort none avoids collecting/sorting the entire posting list before limit.
// No private ABI, indexer process, service activation or index creation.
struct ProcessResult { bool ok = false, bounded = false; QByteArray output; };
ProcessResult balooCommand(const QString &program, const QStringList &arguments,
                          const Request &request, QElapsedTimer &elapsed, qint64 maxOutput) {
    ProcessResult result;
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C.UTF-8");
    environment.remove("DISPLAY");
    environment.remove("WAYLAND_DISPLAY");
    environment.remove("DBUS_SESSION_BUS_ADDRESS");
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(program, arguments, QIODevice::ReadOnly);
    QElapsedTimer timeout;
    timeout.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(10);
        result.output += process.readAllStandardOutput();
        if (request.cancel->load(std::memory_order_relaxed) || elapsed.elapsed() >= request.options.maxElapsedMs ||
            timeout.elapsed() >= 3000 || result.output.size() > maxOutput) {
            result.bounded = true;
            process.kill();
            process.waitForFinished(100);
            break;
        }
    }
    result.output += process.readAllStandardOutput();
    result.ok = !result.bounded && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0 && result.output.size() <= maxOutput;
    return result;
}
QString balooAvailable(const QList<Mount> &mounts, const Request &request, QElapsedTimer &elapsed, QString &reason) {
    auto db = qEnvironmentVariable("BALOO_DB_PATH");
    if (db.isEmpty()) db = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/baloo";
    db = QDir::cleanPath(db);
    if (!QDir::isAbsolutePath(db)) { reason = "Baloo database path is not absolute."; return {}; }
    Descriptor directory(openRoot(QFile::encodeName(db), mounts, request.cancel));
    struct stat index{};
    if (directory.fd < 0 || fstatat(directory.fd, "index", &index, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(index.st_mode) || index.st_size == 0) {
        reason = "No existing readable local Baloo index; using bounded local search.";
        return {};
    }
    auto program = QStandardPaths::findExecutable("baloosearch6");
    if (program.isEmpty()) { reason = "Baloo 6 query tool is unavailable."; return {}; }
    const auto version = balooCommand(program, {"--version"}, request, elapsed, 4096);
    const auto help = balooCommand(program, {"--help"}, request, elapsed, 16384);
    if (!version.ok || !version.output.startsWith("Baloo 6.") || !help.ok || !help.output.contains("--directory") ||
        !help.output.contains("--limit") || !help.output.contains("--sort")) {
        reason = "Installed Baloo query interface is incompatible or unavailable.";
        return {};
    }
    return program;
}
bool balooLiteral(const QString &text) { return !text.contains('"') && !text.contains('\n') && !text.contains('\r'); }
QString indexedSeed(const Criterion &node) {
    if (node.match == "none") return {};
    if (node.match == "all") {
        for (const auto &child : node.children) {
            const auto seed = indexedSeed(child);
            if (!seed.isEmpty()) return seed;
        }
        return {};
    }
    if (node.match == "any") {
        QStringList seeds;
        for (const auto &child : node.children) {
            const auto seed = indexedSeed(child);
            if (seed.isEmpty()) return {};
            seeds.append('(' + seed + ')');
        }
        return seeds.join(" OR ");
    }
    // Name substring semantics differ from Baloo token/prefix semantics, so
    // never use a filename token to exclude portable substring candidates.
    if (node.field == Field::Content && node.op == Op::Contains && balooLiteral(node.value)) return "content:\"" + node.value + '"';
    if (node.field == Field::Kind && node.op == Op::Equals && (node.value == "image" || node.value == "audio" || node.value == "video")) return "type:" + node.value;
    return {};
}
void contentTerms(const Criterion &node, QSet<QString> &terms) {
    if (node.match == "none") return; // absence in an index cannot prove negation
    if (!node.match.isEmpty()) { for (const auto &child : node.children) contentTerms(child, terms); }
    else if (node.field == Field::Content && node.op == Op::Contains && balooLiteral(node.value)) terms.insert(node.value);
}
QList<QByteArray> indexedPaths(const QByteArray &output, const QByteArray &root, bool hidden, bool &malformed) {
    QList<QByteArray> result;
    const auto prefix = root == "/" ? root : root + '/';
    for (const auto &line : output.split('\n')) {
        if (line.isEmpty()) continue;
        if (line.size() > 8192 || line.contains('\0') || !line.startsWith(prefix)) { malformed = true; continue; }
        const auto relative = line.mid(prefix.size());
        bool safe = !relative.isEmpty();
        for (const auto &part : relative.split('/')) {
            if (part.isEmpty() || part == "." || part == ".." || (!hidden && part.startsWith('.'))) { safe = false; break; }
        }
        if (safe) result.append(line);
    }
    return result;
}
}

class LocalFileSearchWorker : public QThread {
    Q_OBJECT
public:
    explicit LocalFileSearchWorker(std::shared_ptr<Shared> state) : state(std::move(state)) {}
signals:
    void batchReady(quint64 generation, const QList<QUrl> &files);
    void statusReady(quint64 generation, const QJsonObject &status);
    void complete(quint64 generation, int count, bool limited, int skipped,
                  const QString &error, bool canceled);
protected:
    void run() override {
        while (true) {
            std::optional<Request> request;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [this] { return state->stopping || state->pending.has_value(); });
                if (state->stopping) return;
                request = std::move(state->pending);
                state->pending.reset();
                state->active = request->cancel;
            }
            scan(*request);
            std::lock_guard lock(state->mutex);
            if (state->active == request->cancel) state->active.reset();
        }
    }
private:
    std::shared_ptr<Shared> state;

    void scan(const Request &request) {
        int count = 0, examined = 0, skipped = 0;
        bool limited = false;
        QString failure;
        Metrics metrics;
        QMimeDatabase mimes;
        Criterion criteria;
        QList<QUrl> batch;
        QString provider = "local", coverage = "selected-local-folder";
        QString detail = "Bounded local metadata and Unicode text search; MIME kinds use filename extensions. No proprietary Spotlight services.";
        bool indexAvailable = false;
        QElapsedTimer elapsed, lastBatch;
        elapsed.start();
        lastBatch.start();
        const auto canceled = [&] { return request.cancel->load(std::memory_order_relaxed); };
        const auto status = [&] {
            emit statusReady(request.generation, {{"provider", provider}, {"coverage", coverage}, {"detail", detail},
                {"indexAvailable", indexAvailable}, {"examinedEntries", examined},
                {"contentBytesRead", QString::number(metrics.contentBytes)}, {"incompleteContentFiles", metrics.incompleteContent},
                {"unavailableMetadata", metrics.unavailableMetadata}, {"limited", limited}});
        };
        const auto finish = [&] {
            if (!canceled()) status();
            if (!canceled() && !batch.isEmpty()) emit batchReady(request.generation, batch);
            emit complete(request.generation, count, limited, skipped, failure, canceled());
        };
        if (canceled()) { finish(); return; }
        const auto json = combinedCriteria(request, failure);
        if (!failure.isEmpty()) { finish(); return; }
        if (json.isEmpty()) { finish(); return; }
        int nodes = 0;
        if (!compile(json, criteria, failure, nodes)) { finish(); return; }
        if (!request.root.isLocalFile() || (!request.root.host().isEmpty() && request.root.host() != "localhost")) {
            failure = QStringLiteral("Recursive filename search is available for local folders.");
            finish();
            return;
        }
        const auto root = QDir::cleanPath(request.root.toLocalFile());
        if (!QDir::isAbsolutePath(root)) {
            failure = QStringLiteral("Choose an absolute local folder for search.");
            finish();
            return;
        }
        QList<Mount> mounts;
        if (!loadMounts(mounts)) {
            failure = QStringLiteral("Cannot verify local filesystem mounts for recursive search.");
            finish();
            return;
        }
        const auto rootPath = QFile::encodeName(root);
        const int descriptor = openRoot(rootPath, mounts, request.cancel);
        if (descriptor < 0) {
            if (!canceled()) failure = readableError(errno);
            finish();
            return;
        }
        Directory directory(fdopendir(descriptor));
        if (!directory) {
            const int code = errno;
            close(descriptor);
            failure = readableError(code);
            finish();
            return;
        }
        if (request.options.provider != LocalFileSearch::Options::Provider::Local) {
            QString reason;
            const auto program = balooAvailable(mounts, request, elapsed, reason);
            indexAvailable = !program.isEmpty();
            if (!program.isEmpty()) {
                const int cap = std::min(request.options.maxEntries, 10000);
                auto seed = indexedSeed(criteria);
                // This enumerates only the selected folder's existing index
                // postings when no positive seed can represent the predicate.
                if (seed.isEmpty()) seed = "modified>=1970-01-01";
                const QStringList arguments{"--sort", "none", "--limit", QString::number(cap),
                    "--directory", root, "--", seed};
                const auto found = balooCommand(program, arguments, request, elapsed, 8 * 1024 * 1024);
                if (found.ok) {
                    provider = "baloo";
                    coverage = "existing-index-only";
                    detail = "Existing Baloo index only: excluded/unindexed files are absent and content may be stale. Indexed content uses word/phrase/prefix matching; readable local text uses substring matching. No indexer is started.";
                    limited |= found.output.count('\n') >= cap;
                    bool malformed = false;
                    auto paths = indexedPaths(found.output, rootPath, request.options.showHidden, malformed);
                    limited |= malformed;
                    QHash<QString, QSet<QByteArray>> contentHits;
                    QSet<QString> terms;
                    contentTerms(criteria, terms);
                    int queried = 0;
                    for (const auto &term : terms) {
                        if (canceled() || elapsed.elapsed() >= request.options.maxElapsedMs) { limited = true; break; }
                        if (++queried > 16) { limited = true; break; }
                        auto contentArguments = arguments;
                        contentArguments.last() = "content:\"" + term + '"';
                        const auto hits = seed == contentArguments.last() ? found : balooCommand(program, contentArguments, request, elapsed, 8 * 1024 * 1024);
                        if (!hits.ok) { limited = true; continue; }
                        limited |= hits.output.count('\n') >= cap;
                        auto &set = contentHits[term];
                        for (const auto &path : indexedPaths(hits.output, rootPath, request.options.showHidden, malformed)) set.insert(path);
                    }
                    limited |= malformed;
                    status();
                    QSet<QByteArray> seen;
                    for (const auto &path : paths) {
                        if (canceled()) break;
                        if (examined >= request.options.maxEntries || elapsed.elapsed() >= request.options.maxElapsedMs) { limited = true; break; }
                        if (seen.contains(path)) continue;
                        seen.insert(path);
                        ++examined;
                        if (!localPath(path, mounts)) { ++skipped; continue; }
                        const auto slash = path.lastIndexOf('/');
                        Descriptor parent(openRoot(slash == 0 ? QByteArray("/") : path.left(slash), mounts, request.cancel));
                        if (parent.fd < 0) { ++skipped; continue; }
                        Entry candidate(parent.fd, path.mid(slash + 1), mimes, request, elapsed, metrics);
                        if (!candidate.valid) { ++skipped; continue; }
                        candidate.path = path;
                        candidate.indexedContent = &contentHits;
                        if (candidate.evaluate(criteria) != Truth::Yes || canceled()) continue;
                        batch.append(QUrl::fromLocalFile(QFile::decodeName(path)));
                        ++count;
                        if (batch.size() >= 64 || lastBatch.elapsed() >= 50) {
                            emit batchReady(request.generation, batch);
                            batch.clear();
                            lastBatch.restart();
                        }
                        if (count >= request.options.maxResults) { limited = true; break; }
                    }
                    finish();
                    return;
                }
                reason = "Baloo query failed or exceeded its time/output budget; using bounded local search.";
                if (found.bounded) limited = true;
            }
            if (request.options.provider == LocalFileSearch::Options::Provider::IndexOnly) {
                provider = "baloo";
                coverage = "existing-index-only";
                detail = reason;
                failure = reason;
                finish();
                return;
            }
            detail = reason;
        }
        status();
        std::vector<Frame> stack;
        stack.push_back({rootPath, std::move(directory)});
        QByteArray preferredPath;
        if (!request.options.preferredPath.isEmpty()) {
            const auto preferred = QFile::encodeName(QDir::cleanPath(request.options.preferredPath));
            if (preferred != rootPath && preferred.startsWith(rootPath == "/" ? rootPath : rootPath + '/')) {
                const int preferredDescriptor = openRoot(preferred, mounts, request.cancel);
                if (preferredDescriptor >= 0) {
                    Directory first(fdopendir(preferredDescriptor));
                    if (first) {
                        preferredPath = preferred;
                        stack.push_back({preferred, std::move(first)});
                    } else close(preferredDescriptor);
                }
            }
        }
        while (!stack.empty() && !canceled()) {
            if (examined >= request.options.maxEntries || elapsed.elapsed() >= request.options.maxElapsedMs) {
                limited = true;
                break;
            }
            auto &frame = stack.back();
            errno = 0;
            const auto *entry = readdir(frame.directory.get());
            if (!entry) {
                if (errno) ++skipped;
                stack.pop_back();
                continue;
            }
            const QByteArray name(entry->d_name);
            if (name == "." || name == "..") continue;
            ++examined;
            if (!request.options.showHidden && name.startsWith('.')) continue;
            const QByteArray path = frame.path == "/" ? '/' + name : frame.path + '/' + name;
            // Do not even stat a remote or pseudo mount entry.
            if (!localPath(path, mounts)) { ++skipped; continue; }
            Entry candidate(dirfd(frame.directory.get()), name, mimes, request, elapsed, metrics);
            if (!candidate.valid) { ++skipped; continue; }
            const bool folder = S_ISDIR(candidate.metadata.st_mode);
            const bool ordinary = folder || S_ISREG(candidate.metadata.st_mode) || S_ISLNK(candidate.metadata.st_mode);
            if (ordinary && candidate.evaluate(criteria) == Truth::Yes && !canceled()) {
                batch.append(QUrl::fromLocalFile(QFile::decodeName(path)));
                ++count;
                if (batch.size() >= 64 || lastBatch.elapsed() >= 50) {
                    emit batchReady(request.generation, batch);
                    batch.clear();
                    lastBatch.restart();
                }
                if (count >= request.options.maxResults) { limited = true; break; }
            }
            // Keep the directory itself discoverable; only its descendants
            // were already scanned first and must not be visited twice.
            if (!folder || canceled() || path == preferredPath) continue;
            if (stack.size() >= 128) { ++skipped; continue; }
            const int child = openat(dirfd(frame.directory.get()), name.constData(),
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { ++skipped; continue; }
            Directory next(fdopendir(child));
            if (!next) { close(child); ++skipped; continue; }
            stack.push_back({path, std::move(next)});
        }
        finish();
    }
};

class LocalFileSearch::Private {
public:
    std::shared_ptr<Shared> state = std::make_shared<Shared>();
    quint64 generation = 0;
    bool running = false;
};

LocalFileSearch::LocalFileSearch(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {
    qRegisterMetaType<QList<QUrl>>();
    auto *worker = new LocalFileSearchWorker(d->state);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &LocalFileSearchWorker::statusReady, this, [this](quint64 generation, const QJsonObject &status) {
        if (generation == d->generation && d->running) emit providerStatus(generation, status);
    }, Qt::QueuedConnection);
    connect(worker, &LocalFileSearchWorker::batchReady, this, [this](quint64 generation, const QList<QUrl> &files) {
        if (generation == d->generation && d->running) emit filesFound(generation, files);
    }, Qt::QueuedConnection);
    connect(worker, &LocalFileSearchWorker::complete, this,
            [this](quint64 generation, int count, bool limited, int skipped, const QString &failure, bool wasCanceled) {
        if (generation != d->generation || !d->running) return;
        d->running = false;
        if (wasCanceled) emit canceled(generation);
        else if (!failure.isEmpty()) emit error(generation, failure);
        else emit finished(generation, count, limited, skipped);
    }, Qt::QueuedConnection);
    worker->start();
}

LocalFileSearch::~LocalFileSearch() {
    {
        std::lock_guard lock(d->state->mutex);
        d->state->stopping = true;
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        if (d->state->pending) d->state->pending->cancel->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
    }
    d->state->wake.notify_one();
    // The independent worker owns its state and deletes itself after finishing.
    // Never join/wait on the UI thread if a local filesystem syscall is slow.
}

quint64 LocalFileSearch::start(const QUrl &root, const QString &filename) {
    return start(root, filename, Options{});
}

quint64 LocalFileSearch::start(const QUrl &root, const QString &filename, const Options &requested) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto previous = d->generation;
    const bool wasRunning = d->running;
    Options options = requested;
    options.maxResults = std::clamp(options.maxResults, 1, 10000);
    options.maxEntries = std::clamp(options.maxEntries, 1, 1000000);
    options.maxElapsedMs = std::clamp(options.maxElapsedMs, 1, 120000);
    options.maxContentBytes = std::clamp(options.maxContentBytes, 1, 4 * 1024 * 1024);
    options.maxTotalContentBytes = std::clamp<qint64>(options.maxTotalContentBytes, 1, 128 * 1024 * 1024);
    const auto generation = ++d->generation;
    d->running = true;
    {
        std::lock_guard lock(d->state->mutex);
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        if (d->state->pending) d->state->pending->cancel->store(true, std::memory_order_relaxed);
        d->state->pending = Request{generation, root, filename.trimmed(), options, std::make_shared<std::atomic_bool>(false)};
    }
    d->state->wake.notify_one();
    if (wasRunning) emit canceled(previous);
    emit started(generation, root, filename.trimmed());
    return generation;
}

void LocalFileSearch::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->running) return;
    {
        std::lock_guard lock(d->state->mutex);
        if (d->state->active) d->state->active->store(true, std::memory_order_relaxed);
        if (d->state->pending) d->state->pending->cancel->store(true, std::memory_order_relaxed);
        d->state->pending.reset();
    }
    d->running = false;
    emit canceled(d->generation);
}

bool LocalFileSearch::validateCriteria(const QJsonObject &criteria, QString *message) {
    QString error;
    Criterion compiled;
    int nodes = 0;
    const bool valid = criteria.isEmpty() || compile(criteria, compiled, error, nodes);
    if (message) *message = error;
    return valid;
}

QJsonObject LocalFileSearch::parseQuery(const QString &query, QString *message) {
    QueryParser parser;
    const auto result = parser.parse(query);
    if (message) *message = parser.error;
    return result;
}

quint64 LocalFileSearch::generation() const { return d->generation; }
bool LocalFileSearch::isRunning() const { return d->running; }

#include "search.moc"
