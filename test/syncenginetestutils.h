/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */
#pragma once

#include "account.h"
#include "creds/abstractcredentials.h"
#include "filesystem.h"
#include "syncengine.h"
#include "syncjournaldb.h"

#include <QDir>
#include <QNetworkReply>
#include <QMap>
#include <QtTest>

static const QUrl sRootUrl("owncloud://somehost/owncloud/remote.php/webdav/");
static const QUrl sBundleRootUrl("owncloud://somehost/remote.php/dav/files/");

inline QString generateEtag() {
    return QString::number(QDateTime::currentDateTime().toMSecsSinceEpoch(), 16);
}
inline QByteArray generateFileId() {
    return QByteArray::number(qrand(), 16);
}

class PathComponents : public QStringList {
public:
    PathComponents(const char *path) : PathComponents{QString::fromUtf8(path)} {}
    PathComponents(const QString &path) : QStringList{path.split('/', QString::SkipEmptyParts)} { }
    PathComponents(const QStringList &pathComponents) : QStringList{pathComponents} { }

    PathComponents parentDirComponents() const {
        return PathComponents{mid(0, size() - 1)};
    }
    PathComponents subComponents() const { return PathComponents{mid(1)}; }
    QString pathRoot() const { return first(); }
    QString fileName() const { return last(); }
};

class FileModifier
{
public:
    virtual ~FileModifier() { }
    virtual void remove(const QString &relativePath) = 0;
    virtual void insert(const QString &relativePath, qint64 size = 64, char contentChar = 'W') = 0;
    virtual void setContents(const QString &relativePath, char contentChar) = 0;
    virtual void appendByte(const QString &relativePath) = 0;
    virtual void mkdir(const QString &relativePath) = 0;
    virtual void rename(const QString &relativePath, const QString &relativeDestinationDirectory) = 0;
};

class DiskFileModifier : public FileModifier
{
    QDir _rootDir;
public:
    DiskFileModifier(const QString &rootDirPath) : _rootDir(rootDirPath) { }
    void remove(const QString &relativePath) override {
        QFileInfo fi{_rootDir.filePath(relativePath)};
        if (fi.isFile())
            QVERIFY(_rootDir.remove(relativePath));
        else
            QVERIFY(QDir{fi.filePath()}.removeRecursively());
    }
    void insert(const QString &relativePath, qint64 size = 64, char contentChar = 'W') override {
        QFile file{_rootDir.filePath(relativePath)};
        QVERIFY(!file.exists());
        file.open(QFile::WriteOnly);
        file.write(QByteArray{}.fill(contentChar, size));
        file.close();
        // Set the mtime 30 seconds in the past, for some tests that need to make sure that the mtime differs.
        OCC::FileSystem::setModTime(file.fileName(), OCC::Utility::qDateTimeToTime_t(QDateTime::currentDateTime().addSecs(-30)));
    }
    void setContents(const QString &relativePath, char contentChar) override {
        QFile file{_rootDir.filePath(relativePath)};
        QVERIFY(file.exists());
        qint64 size = file.size();
        file.open(QFile::WriteOnly);
        file.write(QByteArray{}.fill(contentChar, size));
    }
    void appendByte(const QString &relativePath) override {
        QFile file{_rootDir.filePath(relativePath)};
        QVERIFY(file.exists());
        file.open(QFile::ReadWrite);
        QByteArray contents = file.read(1);
        file.seek(file.size());
        file.write(contents);
    }
    void mkdir(const QString &relativePath) override {
        _rootDir.mkpath(relativePath);
    }
    void rename(const QString &from, const QString &to) override {
        QVERIFY(_rootDir.exists(from));
        QVERIFY(_rootDir.rename(from, to));
    }
};

class FileInfo : public FileModifier
{
public:
    static FileInfo A12_B12_C12_S12() {
        FileInfo fi{QString{}, {
            {QStringLiteral("A"), {
                {QStringLiteral("a1"), 4},
                {QStringLiteral("a2"), 4}
            }},
            {QStringLiteral("B"), {
                {QStringLiteral("b1"), 16},
                {QStringLiteral("b2"), 16}
            }},
            {QStringLiteral("C"), {
                {QStringLiteral("c1"), 24},
                {QStringLiteral("c2"), 24}
            }},
        }};
        FileInfo sharedFolder{QStringLiteral("S"), {
            {QStringLiteral("s1"), 32},
            {QStringLiteral("s2"), 32}
        }};
        sharedFolder.isShared = true;
        sharedFolder.children[QStringLiteral("s1")].isShared = true;
        sharedFolder.children[QStringLiteral("s2")].isShared = true;
        fi.children.insert(sharedFolder.name, std::move(sharedFolder));
        return fi;
    }

    FileInfo() = default;
    FileInfo(const QString &name) : name{name} { }
    FileInfo(const QString &name, qint64 size) : name{name}, isDir{false}, size{size} { }
    FileInfo(const QString &name, qint64 size, char contentChar) : name{name}, isDir{false}, size{size}, contentChar{contentChar} { }
    FileInfo(const QString &name, const std::initializer_list<FileInfo> &children) : name{name} {
        QString p = path();
        for (const auto &source : children) {
            auto &dest = this->children[source.name] = source;
            dest.parentPath = p;
            dest.fixupParentPathRecursively();
        }
    }

    void remove(const QString &relativePath) override {
        const PathComponents pathComponents{relativePath};
        FileInfo *parent = findInvalidatingEtags(pathComponents.parentDirComponents());
        Q_ASSERT(parent);
        parent->children.erase(std::find_if(parent->children.begin(), parent->children.end(),
                [&pathComponents](const FileInfo &fi){ return fi.name == pathComponents.fileName(); }));
    }

    void insert(const QString &relativePath, qint64 size = 64, char contentChar = 'W') override {
        create(relativePath, size, contentChar);
    }

    void setContents(const QString &relativePath, char contentChar) override {
        FileInfo *file = findInvalidatingEtags(relativePath);
        Q_ASSERT(file);
        file->contentChar = contentChar;
    }

    void appendByte(const QString &relativePath) override {
        FileInfo *file = findInvalidatingEtags(relativePath);
        Q_ASSERT(file);
        file->size += 1;
    }

    void mkdir(const QString &relativePath) override {
        createDir(relativePath);
    }

    void rename(const QString &oldPath, const QString &newPath) override {
        const PathComponents newPathComponents{newPath};
        FileInfo *dir = findInvalidatingEtags(newPathComponents.parentDirComponents());
        Q_ASSERT(dir);
        Q_ASSERT(dir->isDir);
        const PathComponents pathComponents{oldPath};
        FileInfo *parent = findInvalidatingEtags(pathComponents.parentDirComponents());
        Q_ASSERT(parent);
        FileInfo fi = parent->children.take(pathComponents.fileName());
        fi.parentPath = dir->path();
        fi.name = newPathComponents.fileName();
        fi.fixupParentPathRecursively();
        dir->children.insert(newPathComponents.fileName(), std::move(fi));
    }

    FileInfo *find(const PathComponents &pathComponents, const bool invalidateEtags = false) {
        if (pathComponents.isEmpty()) {
            if (invalidateEtags)
                etag = generateEtag();
            return this;
        }
        QString childName = pathComponents.pathRoot();
        auto it = children.find(childName);
        if (it != children.end()) {
            auto file = it->find(pathComponents.subComponents(), invalidateEtags);
            if (file && invalidateEtags)
                // Update parents on the way back
                etag = file->etag;
            return file;
        }
        return nullptr;
    }

    FileInfo *createDir(const QString &relativePath) {
        const PathComponents pathComponents{relativePath};
        FileInfo *parent = findInvalidatingEtags(pathComponents.parentDirComponents());
        Q_ASSERT(parent);
        FileInfo &child = parent->children[pathComponents.fileName()] = FileInfo{pathComponents.fileName()};
        child.parentPath = parent->path();
        child.etag = generateEtag();
        return &child;
    }

    FileInfo *create(const QString &relativePath, qint64 size, char contentChar) {
        const PathComponents pathComponents{relativePath};
        FileInfo *parent = findInvalidatingEtags(pathComponents.parentDirComponents());
        Q_ASSERT(parent);
        FileInfo &child = parent->children[pathComponents.fileName()] = FileInfo{pathComponents.fileName(), size};
        child.parentPath = parent->path();
        child.contentChar = contentChar;
        child.etag = generateEtag();
        return &child;
    }

    bool operator<(const FileInfo &other) const {
        return name < other.name;
    }

    bool operator==(const FileInfo &other) const {
        // Consider files to be equal between local<->remote as a user would.
        return name == other.name
            && isDir == other.isDir
            && size == other.size
            && contentChar == other.contentChar
            && children == other.children;
    }

    QString path() const {
        return (parentPath.isEmpty() ? QString() : (parentPath + '/')) + name;
    }

    QString name;
    bool isDir = true;
    bool isShared = false;
    QDateTime lastModified = QDateTime::currentDateTime().addDays(-7);
    QString etag = generateEtag();
    QByteArray fileId = generateFileId();
    qint64 size = 0;
    char contentChar = 'W';

    // Sorted by name to be able to compare trees
    QMap<QString, FileInfo> children;
    QString parentPath;

private:
    FileInfo *findInvalidatingEtags(const PathComponents &pathComponents) {
        return find(pathComponents, true);
    }

    void fixupParentPathRecursively() {
        auto p = path();
        for (auto it = children.begin(); it != children.end(); ++it) {
            Q_ASSERT(it.key() == it->name);
            it->parentPath = p;
            it->fixupParentPathRecursively();
        }
    }

    friend inline QDebug operator<<(QDebug dbg, const FileInfo& fi) {
        return dbg << "{ " << fi.path() << ": " << fi.children;
    }
};

class FakePropfindReply : public QNetworkReply
{
    Q_OBJECT
public:
    QByteArray payload;

    FakePropfindReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        // Don't care about the request and just return a full propfind
        const QString davUri{QStringLiteral("DAV:")};
        const QString ocUri{QStringLiteral("http://owncloud.org/ns")};
        QBuffer buffer{&payload};
        buffer.open(QIODevice::WriteOnly);
        QXmlStreamWriter xml( &buffer );
        xml.writeNamespace(davUri, "d");
        xml.writeNamespace(ocUri, "oc");
        xml.writeStartDocument();
        xml.writeStartElement(davUri, QStringLiteral("multistatus"));
        auto writeFileResponse = [&](const FileInfo &fileInfo) {
            xml.writeStartElement(davUri, QStringLiteral("response"));

            xml.writeTextElement(davUri, QStringLiteral("href"), "/owncloud/remote.php/webdav/" + fileInfo.path());
            xml.writeStartElement(davUri, QStringLiteral("propstat"));
            xml.writeStartElement(davUri, QStringLiteral("prop"));

            if (fileInfo.isDir) {
                xml.writeStartElement(davUri, QStringLiteral("resourcetype"));
                xml.writeEmptyElement(davUri, QStringLiteral("collection"));
                xml.writeEndElement(); // resourcetype
            } else
                xml.writeEmptyElement(davUri, QStringLiteral("resourcetype"));

            auto gmtDate = fileInfo.lastModified.toTimeZone(QTimeZone("GMT"));
            auto stringDate = gmtDate.toString("ddd, dd MMM yyyy HH:mm:ss 'GMT'");
            xml.writeTextElement(davUri, QStringLiteral("getlastmodified"), stringDate);
            xml.writeTextElement(davUri, QStringLiteral("getcontentlength"), QString::number(fileInfo.size));
            xml.writeTextElement(davUri, QStringLiteral("getetag"), fileInfo.etag);
            xml.writeTextElement(ocUri, QStringLiteral("permissions"), fileInfo.isShared ? QStringLiteral("SRDNVCKW") : QStringLiteral("RDNVCKW"));
            xml.writeTextElement(ocUri, QStringLiteral("id"), fileInfo.fileId);
            xml.writeEndElement(); // prop
            xml.writeTextElement(davUri, QStringLiteral("status"), "HTTP/1.1 200 OK");
            xml.writeEndElement(); // propstat
            xml.writeEndElement(); // response
        };

        Q_ASSERT(request.url().path().startsWith(sRootUrl.path()));
        QString fileName = request.url().path().mid(sRootUrl.path().length());
        const FileInfo *fileInfo = remoteRootFileInfo.find(fileName);
        Q_ASSERT(fileInfo);

        writeFileResponse(*fileInfo);
        foreach(const FileInfo &childFileInfo, fileInfo->children)
           writeFileResponse(childFileInfo);
        xml.writeEndElement(); // multistatus
        xml.writeEndDocument();

        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
        setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 207);
        setFinished(true);
        emit metaDataChanged();
        if (bytesAvailable())
            emit readyRead();
        emit finished();
    }

    void abort() override { }

    qint64 bytesAvailable() const override { return payload.size() + QIODevice::bytesAvailable(); }
    qint64 readData(char *data, qint64 maxlen) override {
        qint64 len = std::min(qint64{payload.size()}, maxlen);
        strncpy(data, payload.constData(), len);
        payload.remove(0, len);
        return len;
    }
};

class FakePutReply : public QNetworkReply
{
    Q_OBJECT
    FileInfo *fileInfo;
public:
    FakePutReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, const QByteArray &putPayload, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        Q_ASSERT(request.url().path().startsWith(sRootUrl.path()));
        QString fileName = request.url().path().mid(sRootUrl.path().length());
        if ((fileInfo = remoteRootFileInfo.find(fileName))) {
            fileInfo->size = putPayload.size();
            fileInfo->contentChar = putPayload.at(0);
        } else {
            // Assume that the file is filled with the same character
            fileInfo = remoteRootFileInfo.create(fileName, putPayload.size(), putPayload.at(0));
        }

        if (!fileInfo) {
            abort();
            return;
        }
        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setRawHeader("OC-ETag", fileInfo->etag.toLatin1());
        setRawHeader("ETag", fileInfo->etag.toLatin1());
        setRawHeader("X-OC-MTime", "accepted"); // Prevents Q_ASSERT(!_runningNow) since we'll call PropagateItemJob::done twice in that case.
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        emit metaDataChanged();
        emit finished();
    }

    void abort() override { }
    qint64 readData(char *, qint64) override { return 0; }
};

class FakeMkcolReply : public QNetworkReply
{
    Q_OBJECT
    FileInfo *fileInfo;
public:
    FakeMkcolReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        Q_ASSERT(request.url().path().startsWith(sRootUrl.path()));
        QString fileName = request.url().path().mid(sRootUrl.path().length());
        fileInfo = remoteRootFileInfo.createDir(fileName);

        if (!fileInfo) {
            abort();
            return;
        }
        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setRawHeader("OC-FileId", fileInfo->fileId);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 201);
        emit metaDataChanged();
        emit finished();
    }

    void abort() override { }
    qint64 readData(char *, qint64) override { return 0; }
};

class FakeDeleteReply : public QNetworkReply
{
    Q_OBJECT
public:
    FakeDeleteReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        Q_ASSERT(request.url().path().startsWith(sRootUrl.path()));
        QString fileName = request.url().path().mid(sRootUrl.path().length());
        remoteRootFileInfo.remove(fileName);
        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 204);
        emit metaDataChanged();
        emit finished();
    }

    void abort() override { }
    qint64 readData(char *, qint64) override { return 0; }
};

class FakeMoveReply : public QNetworkReply
{
    Q_OBJECT
public:
    FakeMoveReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        Q_ASSERT(request.url().path().startsWith(sRootUrl.path()));
        QString fileName = request.url().path().mid(sRootUrl.path().length());
        QString destPath = request.rawHeader("Destination");
        Q_ASSERT(destPath.startsWith(sRootUrl.path()));
        QString dest = destPath.mid(sRootUrl.path().length());
        remoteRootFileInfo.rename(fileName, dest);
        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 201);
        emit metaDataChanged();
        emit finished();
    }

    void abort() override { }
    qint64 readData(char *, qint64) override { return 0; }
};

class FakeGetReply : public QNetworkReply
{
    Q_OBJECT
public:
    const FileInfo *fileInfo;
    QByteArray payload;

    FakeGetReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);

        Q_ASSERT(request.url().path().startsWith(sRootUrl.path()));
        QString fileName = request.url().path().mid(sRootUrl.path().length());
        fileInfo = remoteRootFileInfo.find(fileName);
        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        payload.fill(fileInfo->contentChar, fileInfo->size);
        setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        setRawHeader("OC-ETag", fileInfo->etag.toLatin1());
        setRawHeader("ETag", fileInfo->etag.toLatin1());
        setRawHeader("OC-FileId", fileInfo->fileId);
        emit metaDataChanged();
        if (bytesAvailable())
            emit readyRead();
        emit finished();
    }

    void abort() override { }
    qint64 bytesAvailable() const override { return payload.size() + QIODevice::bytesAvailable(); }

    qint64 readData(char *data, qint64 maxlen) override {
        qint64 len = std::min(qint64{payload.size()}, maxlen);
        strncpy(data, payload.constData(), len);
        payload.remove(0, len);
        return len;
    }
};


class FakeErrorReply : public QNetworkReply
{
    Q_OBJECT
public:
    FakeErrorReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly);
        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 500);
        emit metaDataChanged();
        emit finished();
    }

    void abort() override { }
    qint64 readData(char *, qint64) override { return 0; }
};

class FakeBundlePOSTReply : public QNetworkReply
{
    Q_OBJECT
    FileInfo *fileInfo;
    QByteArray payload;
public:
    FakeBundlePOSTReply(FileInfo &remoteRootFileInfo, QNetworkAccessManager::Operation op, const QNetworkRequest &request, const QByteArray &postPayload, QObject *parent)
    : QNetworkReply{parent} {
        setRequest(request);
        QUrl rawUrl = request.url();
        QString bundlePath(sBundleRootUrl.path()+rawUrl.userName());
        setUrl(rawUrl);
        setOperation(op);
        open(QIODevice::ReadOnly);
        const QString davUri{QStringLiteral("DAV:")};
        const QString ocUri{QStringLiteral("http://owncloud.org/ns")};
        const QString sabUri{QStringLiteral("http://sabredav.org/ns")};
        QBuffer buffer{&payload};
        buffer.open(QIODevice::WriteOnly);
        QXmlStreamWriter xml( &buffer );
        xml.writeNamespace(davUri, "d");
        xml.writeNamespace(ocUri, "o");
        xml.writeNamespace(sabUri, "s");

        auto writeFileResponse = [&](const FileInfo &fileInfo) {
            xml.writeStartElement(davUri, QStringLiteral("response"));

            //TODO: no need for X-OC-PATH, href could contain that, fix client/server
            xml.writeTextElement(davUri, QStringLiteral("href"), bundlePath);
            xml.writeStartElement(davUri, QStringLiteral("propstat"));
            xml.writeStartElement(davUri, QStringLiteral("prop"));

            xml.writeTextElement(davUri, QStringLiteral("oc-etag"), fileInfo.etag);
            xml.writeTextElement(davUri, QStringLiteral("etag"), fileInfo.etag);
            xml.writeTextElement(davUri, QStringLiteral("oc-fileid"), fileInfo.fileId);
            xml.writeTextElement(davUri, QStringLiteral("x-oc-mtime"), QStringLiteral("accepted"));

            //TODO: this slash to be fixed on client/server
            xml.writeTextElement(davUri, QStringLiteral("oc-path"), "/"+fileInfo.path());
            xml.writeEndElement(); // prop
            xml.writeTextElement(davUri, QStringLiteral("status"), "HTTP/1.1 200 OK");
            xml.writeEndElement(); // propstat
            xml.writeEndElement(); // response
        };
        auto writeFileErrorResponse = [&](const FileInfo &fileInfo, const QString &exception, const QString &message, const QString &status) {
            xml.writeStartElement(davUri, QStringLiteral("response"));

            //TODO: no need for X-OC-PATH, href could contain that, fix client/server
            xml.writeTextElement(davUri, QStringLiteral("href"), bundlePath);
            xml.writeStartElement(davUri, QStringLiteral("propstat"));
            xml.writeStartElement(davUri, QStringLiteral("prop"));
            xml.writeStartElement(davUri, QStringLiteral("error"));
            xml.writeTextElement(sabUri, QStringLiteral("exception"), exception);
            xml.writeTextElement(sabUri, QStringLiteral("message"), message);
            xml.writeEndElement(); // error

            //TODO: this slash to be fixed on client/server
            xml.writeTextElement(davUri, QStringLiteral("oc-path"), "/"+fileInfo.path());
            xml.writeEndElement(); // prop
            xml.writeTextElement(davUri, QStringLiteral("status"), status);
            xml.writeEndElement(); // propstat
            xml.writeEndElement(); // response
        };


        if ("erroruser" == rawUrl.userName()) {
            xml.writeStartDocument();
            xml.writeStartElement(davUri, QStringLiteral("error"));
            xml.writeTextElement(sabUri, QStringLiteral("exception"), QStringLiteral("OCA\\DAV\\Connector\\Sabre\\Exception\\Forbidden"));
            xml.writeTextElement(sabUri, QStringLiteral("message"), QStringLiteral("URL endpoint has to be instance of \\OCA\\DAV\\Files\\FilesHome"));
            xml.writeTextElement(ocUri, QStringLiteral("retry"), QStringLiteral("false"));
            xml.writeTextElement(ocUri, QStringLiteral("reason"), QStringLiteral("URL endpoint has to be instance of \\OCA\\DAV\\Files\\FilesHome"));
            xml.writeEndElement(); // error
            xml.writeEndDocument();
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 403);
        } else {
            Q_ASSERT(request.url().path().endsWith(bundlePath));
            xml.writeStartDocument();
            xml.writeStartElement(davUri, QStringLiteral("multistatus"));

            //multipart parsing
            QString headerSectEnd = "\r\n\r\n";
            QString headerEnd = "\r\n";
            QString headerOcMethod = "X-OC-Method: ";
            QString headerConLen = "Content-Length: ";
            QString headerOcPath = "X-OC-Path: ";
            int indexOfBody = 0;
            QChar contentChar;

            while(postPayload.indexOf(headerSectEnd,indexOfBody) + headerSectEnd.length() >=indexOfBody) {
                //find oc-method
                int indexOfheaderEnd = postPayload.indexOf(headerOcMethod,indexOfBody) + headerOcMethod.length();
                int indexOfheaderBodyEnd = postPayload.indexOf(headerEnd,indexOfheaderEnd);
                Q_ASSERT(postPayload.mid(indexOfheaderEnd,indexOfheaderBodyEnd-indexOfheaderEnd) == QString("PUT"));

                //find oc-path
                indexOfheaderEnd = postPayload.indexOf(headerOcPath,indexOfBody) + headerOcPath.length();
                indexOfheaderBodyEnd = postPayload.indexOf(headerEnd,indexOfheaderEnd)-1;
                QString filePath(postPayload.mid(indexOfheaderEnd+1,indexOfheaderBodyEnd-indexOfheaderEnd));

                //find content-length
                indexOfheaderEnd = postPayload.indexOf(headerConLen,indexOfBody) + headerConLen.length();
                indexOfheaderBodyEnd = postPayload.indexOf(headerEnd,indexOfheaderEnd);
                QString fileSize(postPayload.mid(indexOfheaderEnd,indexOfheaderBodyEnd-indexOfheaderEnd));


                //find body content and extract first letter
                indexOfheaderEnd = postPayload.indexOf(headerSectEnd,indexOfBody) + headerSectEnd.length();
                indexOfBody = indexOfheaderEnd+1;
                contentChar = postPayload.at(indexOfheaderEnd+1);

                if ((fileInfo = remoteRootFileInfo.find(filePath))) {
                    fileInfo->size = fileSize.toInt();
                    fileInfo->contentChar = contentChar.toAscii();
                } else {
                    // Assume that the file is filled with the same character
                    fileInfo = remoteRootFileInfo.create(filePath, fileSize.toInt(), contentChar.toAscii());
                }

                if (!fileInfo) {
                    abort();
                    return;
                }

                if (filePath.endsWith("normalerrorfile")){
                    writeFileErrorResponse(*fileInfo, QStringLiteral("Sabre\\DAV\\Exception\\BadRequest"), QStringLiteral("Method not allowed - file exists - update of the file is not supported!"), QStringLiteral("HTTP/1.1 400 Bad Request"));
                } else if (filePath.endsWith("fatalerrorfile")){
                    writeFileErrorResponse(*fileInfo, QStringLiteral("Sabre\\DAV\\Exception\\ServiceUnavailable"), QStringLiteral("Failed to check file size"), QStringLiteral("HTTP/1.1 503 Service Unavailable"));
                } else if (filePath.endsWith("softerrorfile")){
                    writeFileErrorResponse(*fileInfo, QStringLiteral("OCA\\DAV\\Connector\\Sabre\\Exception\\FileLocked"), QStringLiteral("Target file is locked by another process."), QStringLiteral("HTTP/1.1 423 Locked (WebDAV; RFC 4918)"));
                } else {
                    writeFileResponse(*fileInfo);
                }
            }
            xml.writeEndElement(); // multistatus
            xml.writeEndDocument();
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 207);
            setFinished(true);
        }

        QMetaObject::invokeMethod(this, "respond", Qt::QueuedConnection);
    }

    Q_INVOKABLE void respond() {
        setHeader(QNetworkRequest::ContentTypeHeader, "application/xml; charset=utf-8");
        setHeader(QNetworkRequest::ContentLengthHeader, payload.size());
        emit metaDataChanged();
        if (bytesAvailable())
            emit readyRead();
        emit finished();
    }

    void abort() override { }

    qint64 bytesAvailable() const override { return payload.size() + QIODevice::bytesAvailable(); }
    qint64 readData(char *data, qint64 maxlen) override {
        qint64 len = std::min(qint64{payload.size()}, maxlen);
        strncpy(data, payload.constData(), len);
        payload.remove(0, len);
        return len;
    }
};

class FakeQNAM : public QNetworkAccessManager
{
    FileInfo _remoteRootFileInfo;
    QStringList _errorPaths;
public:
    FakeQNAM(FileInfo initialRoot) : _remoteRootFileInfo{std::move(initialRoot)} { }
    FileInfo &currentRemoteState() { return _remoteRootFileInfo; }
    QStringList &errorPaths() { return _errorPaths; }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                                         QIODevice *outgoingData = 0) {
        const QString fileName = request.url().path().mid(sRootUrl.path().length());
        if (_errorPaths.contains(fileName))
            return new FakeErrorReply{op, request, this};

        auto verb = request.attribute(QNetworkRequest::CustomVerbAttribute);
        if (verb == QLatin1String("PROPFIND"))
            // Ignore outgoingData always returning somethign good enough, works for now.
            return new FakePropfindReply{_remoteRootFileInfo, op, request, this};
        else if (verb == QLatin1String("GET"))
            return new FakeGetReply{_remoteRootFileInfo, op, request, this};
        else if (verb == QLatin1String("PUT"))
            return new FakePutReply{_remoteRootFileInfo, op, request, outgoingData->readAll(), this};
        else if (verb == QLatin1String("MKCOL"))
            return new FakeMkcolReply{_remoteRootFileInfo, op, request, this};
        else if (verb == QLatin1String("DELETE"))
            return new FakeDeleteReply{_remoteRootFileInfo, op, request, this};
        else if (verb == QLatin1String("MOVE"))
            return new FakeMoveReply{_remoteRootFileInfo, op, request, this};
        else if (op == QNetworkAccessManager::PostOperation) {
            return new FakeBundlePOSTReply{_remoteRootFileInfo, op, request, outgoingData->readAll(), this};
        } else {
            qDebug() << verb << outgoingData;
            Q_UNREACHABLE();
        }
    }
};

class FakeCredentials : public OCC::AbstractCredentials
{
    QNetworkAccessManager *_qnam;
public:
    FakeCredentials(QNetworkAccessManager *qnam) : _qnam{qnam} { }
    virtual bool changed(AbstractCredentials *) const { return false; }
    virtual QString authType() const { return "test"; }
    virtual QString user() const { return "admin"; }
    virtual QNetworkAccessManager* getQNAM() const { return _qnam; }
    virtual bool ready() const { return true; }
    virtual void fetchFromKeychain() { }
    virtual void askFromUser() { }
    virtual bool stillValid(QNetworkReply *) { return true; }
    virtual void persist() { }
    virtual void invalidateToken() { }
    virtual void forgetSensitiveData() { }
};

class FakeFolder
{
    QTemporaryDir _tempDir;
    DiskFileModifier _localModifier;
    // FIXME: Clarify ownership, double delete
    FakeQNAM *_fakeQnam;
    OCC::AccountPtr _account;
    std::unique_ptr<OCC::SyncJournalDb> _journalDb;
    std::unique_ptr<OCC::SyncEngine> _syncEngine;

public:
    FakeFolder(const FileInfo &fileTemplate)
        : _localModifier(_tempDir.path())
    {
        // Needs to be done once
        OCC::SyncEngine::minimumFileAgeForUpload = 0;
        csync_set_log_level(11);

        QDir rootDir{_tempDir.path()};
        toDisk(rootDir, fileTemplate);

        _fakeQnam = new FakeQNAM(fileTemplate);
        _account = OCC::Account::create();
        _account->setUrl(QUrl(QStringLiteral("http://admin:admin@localhost/owncloud")));
        _account->setCredentials(new FakeCredentials{_fakeQnam});

        _journalDb.reset(new OCC::SyncJournalDb(localPath()));
        _syncEngine.reset(new OCC::SyncEngine(_account, localPath(), sRootUrl, "", _journalDb.get()));

        // A new folder will update the local file state database on first sync.
        // To have a state matching what users will encounter, we have to a sync
        // using an identical local/remote file tree first.
        syncOnce();
    }

    OCC::SyncEngine &syncEngine() const { return *_syncEngine; }

    FileModifier &localModifier() { return _localModifier; }
    FileModifier &remoteModifier() { return _fakeQnam->currentRemoteState(); }
    FileInfo currentLocalState() {
        QDir rootDir{_tempDir.path()};
        FileInfo rootTemplate;
        fromDisk(rootDir, rootTemplate);
        return rootTemplate;
    }

    FileInfo currentRemoteState() { return _fakeQnam->currentRemoteState(); }

    QStringList &serverErrorPaths() { return _fakeQnam->errorPaths(); }

    QString localPath() const {
        // SyncEngine wants a trailing slash
        if (_tempDir.path().endsWith('/'))
            return _tempDir.path();
        return _tempDir.path() + '/';
    }

    void scheduleSync() {
        // Have to be done async, else, an error before exec() does not terminate the event loop.
        QMetaObject::invokeMethod(_syncEngine.get(), "startSync", Qt::QueuedConnection);
    }

    void execUntilBeforePropagation() {
        QSignalSpy spy(_syncEngine.get(), SIGNAL(aboutToPropagate(SyncFileItemVector&)));
        QVERIFY(spy.wait());
    }

    void execUntilItemCompleted(const QString &relativePath) {
        QSignalSpy spy(_syncEngine.get(), SIGNAL(itemCompleted(const SyncFileItem &, const PropagatorJob &)));
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 5000) {
            spy.clear();
            QVERIFY(spy.wait());
            for(const QList<QVariant> &args : spy) {
                auto item = args[0].value<OCC::SyncFileItem>();
                if (item.destination() == relativePath)
                    return;
            }
        }
        QVERIFY(false);
    }

    void execUntilFinished() {
        QSignalSpy spy(_syncEngine.get(), SIGNAL(finished(bool)));
        QVERIFY(spy.wait());
    }

    void syncOnce() {
        scheduleSync();
        execUntilFinished();
    }

    OCC::AccountPtr getAccount() {
        return _account;
    }

private:
    static void toDisk(QDir &dir, const FileInfo &templateFi) {
        foreach (const FileInfo &child, templateFi.children) {
            if (child.isDir) {
                QDir subDir(dir);
                dir.mkdir(child.name);
                subDir.cd(child.name);
                toDisk(subDir, child);
            } else {
                QFile file{dir.filePath(child.name)};
                file.open(QFile::WriteOnly);
                file.write(QByteArray{}.fill(child.contentChar, child.size));
                file.close();
                OCC::FileSystem::setModTime(file.fileName(), OCC::Utility::qDateTimeToTime_t(child.lastModified));
            }
        }
    }

    static void fromDisk(QDir &dir, FileInfo &templateFi) {
        foreach (const QFileInfo &diskChild, dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
            if (diskChild.isDir()) {
                QDir subDir = dir;
                subDir.cd(diskChild.fileName());
                templateFi.children.insert(diskChild.fileName(), FileInfo{diskChild.fileName()});
                fromDisk(subDir, templateFi.children.last());
            } else {
                QFile f{diskChild.filePath()};
                f.open(QFile::ReadOnly);
                char contentChar = f.read(1).at(0);
                templateFi.children.insert(diskChild.fileName(), FileInfo{diskChild.fileName(), diskChild.size(), contentChar});
            }
        }
    }
};

// QTest::toString overloads
namespace OCC {
    inline char *toString(const SyncFileStatus &s) {
        return QTest::toString(QString("SyncFileStatus(" + s.toSocketAPIString() + ")"));
    }
}

inline void addFiles(QStringList &dest, const FileInfo &fi)
{
    if (fi.isDir) {
        dest += QString("%1 - dir").arg(fi.name);
        foreach (const FileInfo &fi, fi.children)
            addFiles(dest, fi);
    } else {
        dest += QString("%1 - %2 %3-bytes").arg(fi.name).arg(fi.size).arg(fi.contentChar);
    }
}

inline char *toString(const FileInfo &fi)
{
    QStringList files;
    foreach (const FileInfo &fi, fi.children)
        addFiles(files, fi);
    return QTest::toString(QString("FileInfo with %1 files(%2)").arg(files.size()).arg(files.join(", ")));
}
