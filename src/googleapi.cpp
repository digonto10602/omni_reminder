#include "googleapi.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

namespace Google {

Endpoints Endpoints::fromEnvironment() {
    Endpoints e;
    const QString base = qEnvironmentVariable("OMNI_REMINDER_GOOGLE_BASE");
    if (!base.isEmpty()) {
        e.auth = base + "/auth";
        e.token = base + "/token";
        e.revoke = base + "/revoke";
        e.calendar = base + "/calendar/v3";
        e.tasks = base + "/tasks/v1";
    }
    return e;
}

QString Client::configDirectory() {
    const QString overridden = qEnvironmentVariable("OMNI_REMINDER_CONFIG_DIR");
    if (!overridden.isEmpty())
        return overridden;
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + "/.config";
    return base + "/omni-reminder";
}

QString Client::defaultPath() {
    return configDirectory() + "/google-client.json";
}

bool Client::load(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("No Google OAuth client at %1 (see the README: Google sync setup)").arg(path);
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    // Google's download wraps a Desktop client in "installed".
    const QJsonObject installed = root.contains("installed") ? root.value("installed").toObject() : root;
    id = installed.value("client_id").toString();
    secret = installed.value("client_secret").toString();
    if (id.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1 has no client_id; download the JSON of a \"Desktop app\" client").arg(path);
        return false;
    }
    return true;
}

// ---------- token store ----------

TokenStore::TokenStore(const QString &mode)
    : m_mode(mode.isEmpty() ? qEnvironmentVariable("OMNI_REMINDER_TOKEN_STORE") : mode) {}

QString TokenStore::filePath() {
    return Client::configDirectory() + "/google-token.json";
}

bool TokenStore::keyringAllowed() const {
    return m_mode != "file" && !QStandardPaths::findExecutable("secret-tool").isEmpty();
}

static const QStringList keyringAttributes{"service", "omni-reminder", "account", "google"};

static bool runSecretTool(const QStringList &arguments, const QByteArray &input, QByteArray *output) {
    QProcess tool;
    tool.start("secret-tool", arguments);
    if (!tool.waitForStarted(3000))
        return false;
    if (!input.isNull()) {
        tool.write(input);
        tool.closeWriteChannel();
    }
    // An unlock prompt may appear; give the user time to answer it.
    if (!tool.waitForFinished(60000)) {
        tool.kill();
        return false;
    }
    if (output)
        *output = tool.readAllStandardOutput();
    return tool.exitStatus() == QProcess::NormalExit && tool.exitCode() == 0;
}

bool TokenStore::save(const QString &refreshToken, QString *error) {
    if (keyringAllowed() &&
        runSecretTool(QStringList{"store", "--label=Omni Reminder (Google sync)"} + keyringAttributes,
                      refreshToken.toUtf8(), nullptr)) {
        QFile::remove(filePath()); // only one copy
        return true;
    }
    if (m_mode == "keyring") {
        if (error)
            *error = QStringLiteral("The desktop keyring did not take the token (secret-tool failed)");
        return false;
    }
    QDir().mkpath(Client::configDirectory());
    QFile::setPermissions(Client::configDirectory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(filePath());
        return false;
    }
    // Owner-only before any secret is written.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    file.write(QJsonDocument(QJsonObject{{"refresh_token", refreshToken},
                                         {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}})
                   .toJson());
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Cannot save %1").arg(filePath());
        return false;
    }
    QFile::setPermissions(filePath(), QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

QString TokenStore::load() const {
    if (keyringAllowed()) {
        QByteArray out;
        if (runSecretTool(QStringList{"lookup"} + keyringAttributes, QByteArray(), &out) && !out.trimmed().isEmpty())
            return QString::fromUtf8(out.trimmed());
    }
    if (m_mode == "keyring")
        return {};
    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("refresh_token").toString();
}

void TokenStore::clear() {
    if (keyringAllowed())
        runSecretTool(QStringList{"clear"} + keyringAttributes, QByteArray(), nullptr);
    QFile::remove(filePath());
}

QString TokenStore::location() const {
    if (keyringAllowed()) {
        QByteArray out;
        if (runSecretTool(QStringList{"lookup"} + keyringAttributes, QByteArray(), &out) && !out.trimmed().isEmpty())
            return QStringLiteral("desktop keyring (Secret Service)");
    }
    return QFileInfo::exists(filePath()) ? filePath() : QString();
}

// ---------- HTTP ----------

QJsonObject Response::json() const {
    return QJsonDocument::fromJson(body).object();
}

Response send(QNetworkAccessManager &network, const QByteArray &method, const QUrl &url, const QByteArray &body,
              const QByteArray &contentType, const QByteArray &bearer, int timeoutMs) {
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!contentType.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    if (!bearer.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + bearer);
    QNetworkReply *reply = network.sendCustomRequest(request, method, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, reply, &QNetworkReply::abort);
    guard.start(timeoutMs + 5000);
    if (!reply->isFinished())
        loop.exec();
    Response response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    if (response.status == 0)
        response.networkError = reply->errorString();
    reply->deleteLater();
    return response;
}

// application/x-www-form-urlencoded by hand: every byte but the unreserved
// ones is escaped, so a "+" or "/" in a code or secret arrives intact.
static QByteArray form(const QList<QPair<QString, QString>> &fields) {
    QByteArrayList parts;
    for (const auto &field : fields)
        parts << QUrl::toPercentEncoding(field.first) + "=" + QUrl::toPercentEncoding(field.second);
    return parts.join('&');
}

static Tokens tokensFrom(const Response &response) {
    Tokens tokens;
    if (response.status == 0 || response.status >= 500 || response.status == 429) {
        tokens.offline = true;
        tokens.error = response.status == 0 ? response.networkError
                                            : QStringLiteral("Google answered %1").arg(response.status);
        return tokens;
    }
    const QJsonObject object = response.json();
    if (!response.ok()) {
        const QString code = object.value("error").toString();
        tokens.invalidGrant = code == "invalid_grant" || code == "invalid_client" || code == "unauthorized_client";
        tokens.error = code.isEmpty() ? QStringLiteral("token request failed (%1)").arg(response.status)
                                      : code + ": " + object.value("error_description").toString();
        return tokens;
    }
    tokens.access = object.value("access_token").toString();
    tokens.refresh = object.value("refresh_token").toString();
    tokens.scope = object.value("scope").toString();
    tokens.expires = QDateTime::currentDateTimeUtc().addSecs(object.value("expires_in").toInt(3600) - 60);
    if (tokens.access.isEmpty())
        tokens.error = QStringLiteral("Google sent no access token");
    return tokens;
}

Tokens refresh(QNetworkAccessManager &network, const Endpoints &endpoints, const Client &client,
               const QString &refreshToken) {
    QList<QPair<QString, QString>> fields{{"client_id", client.id},
                                          {"refresh_token", refreshToken},
                                          {"grant_type", "refresh_token"}};
    if (!client.secret.isEmpty())
        fields.append({"client_secret", client.secret});
    return tokensFrom(send(network, "POST", QUrl(endpoints.token), form(fields),
                           "application/x-www-form-urlencoded"));
}

QString makeVerifier() {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString verifier;
    for (int i = 0; i < 64; ++i)
        verifier += QChar(alphabet[QRandomGenerator::system()->bounded(66)]);
    return verifier;
}

QString challengeFor(const QString &verifier) {
    return QString::fromLatin1(QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                                   .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

static QString pageFor(bool ok, const QString &detail) {
    return QStringLiteral("<!doctype html><meta charset=utf-8><title>Omni Reminder</title>"
                          "<body style=\"font:16px sans-serif;background:#111;color:#eee;padding:3em\">"
                          "<h2>%1</h2><p>%2</p></body>")
        .arg(ok ? "Omni Reminder is signed in" : "Omni Reminder could not sign in", detail.toHtmlEscaped());
}

Tokens login(const Endpoints &endpoints, const Client &client, const std::function<bool(const QUrl &)> &open,
             const std::function<void(const QString &)> &say, int timeoutMs) {
    Tokens result;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        result.error = QStringLiteral("Cannot listen on 127.0.0.1: %1").arg(server.errorString());
        return result;
    }
    const QString redirect = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    const QString verifier = makeVerifier();
    const QString state = makeVerifier().left(32);

    // access_type=offline asks for a refresh token; prompt=consent makes
    // Google send a new one even if this account signed in before.
    const QByteArray query = form({{"client_id", client.id},
                                   {"redirect_uri", redirect},
                                   {"response_type", "code"},
                                   {"scope", Scopes},
                                   {"code_challenge", challengeFor(verifier)},
                                   {"code_challenge_method", "S256"},
                                   {"state", state},
                                   {"access_type", "offline"},
                                   {"prompt", "consent"}});
    const QUrl url = QUrl::fromEncoded(endpoints.auth.toUtf8() + "?" + query);

    say(QStringLiteral("Opening Google's sign-in page in your browser. If it does not open, visit:\n%1")
            .arg(url.toString(QUrl::FullyEncoded)));
    if (!open(url))
        say(QStringLiteral("(Could not open a browser; copy the address above.)"));

    QString code, failure;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        failure = QStringLiteral("No answer from the browser within %1 minutes").arg(timeoutMs / 60000);
        loop.quit();
    });
    QObject::connect(&server, &QTcpServer::newConnection, &loop, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::readyRead, &loop, [&, socket] {
                const QByteArray head = socket->peek(8192);
                if (!head.contains("\r\n\r\n"))
                    return;
                const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
                const QUrl target(QString::fromLatin1(requestLine.value(1)));
                const QUrlQuery answer(target);
                QString page;
                if (target.path() != "/" || (!answer.hasQueryItem("code") && !answer.hasQueryItem("error"))) {
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (answer.queryItemValue("state") != state) {
                    failure = QStringLiteral("The sign-in answer did not match this request (state)");
                } else if (answer.hasQueryItem("error")) {
                    failure = QStringLiteral("Google said: %1").arg(answer.queryItemValue("error"));
                } else {
                    code = answer.queryItemValue("code", QUrl::FullyDecoded);
                }
                page = pageFor(failure.isEmpty(), failure.isEmpty()
                                                     ? QStringLiteral("You can close this tab and return to the terminal.")
                                                     : failure);
                const QByteArray bytes = page.toUtf8();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                              QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
                socket->flush();
                socket->disconnectFromHost();
                loop.quit();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    timeout.start(timeoutMs);
    loop.exec();
    server.close();
    if (code.isEmpty()) {
        result.error = failure.isEmpty() ? QStringLiteral("Sign-in was not completed") : failure;
        return result;
    }

    QNetworkAccessManager network;
    QList<QPair<QString, QString>> fields{{"code", code},
                                          {"client_id", client.id},
                                          {"redirect_uri", redirect},
                                          {"grant_type", "authorization_code"},
                                          {"code_verifier", verifier}};
    if (!client.secret.isEmpty())
        fields.append({"client_secret", client.secret});
    result = tokensFrom(send(network, "POST", QUrl(endpoints.token), form(fields), "application/x-www-form-urlencoded"));
    if (result.ok() && result.refresh.isEmpty())
        result.error = QStringLiteral("Google sent no refresh token; run `omni-reminder sync login` again");
    // The consent page lets the user untick each permission; both are needed.
    if (result.ok() && !result.scope.isEmpty()) {
        const QStringList granted = result.scope.split(' ', Qt::SkipEmptyParts);
        for (const QString &needed : QString::fromLatin1(Scopes).split(' '))
            if (!granted.contains(needed)) {
                result.error = QStringLiteral("Google did not grant %1; run `omni-reminder sync login` again and "
                                              "leave both boxes ticked").arg(needed);
                result.access.clear();
            }
    }
    return result;
}

void revoke(QNetworkAccessManager &network, const Endpoints &endpoints, const QString &token) {
    const QUrl url = QUrl::fromEncoded(endpoints.revoke.toUtf8() + "?" + form({{"token", token}}));
    send(network, "POST", url, {}, "application/x-www-form-urlencoded", {}, 10000);
}

} // namespace Google
