#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

// Everything that talks to Google itself: where the endpoints are, the OAuth
// client the user created, where the sign-in is kept, and the sign-in flow.
// All calls here block (with their own event loop), so they belong on the
// sync thread or in a command-line process, never on the window's thread.
namespace Google {

// Least privilege: calendar.app.created reaches only calendars this app
// created (and their events), tasks the user's task lists.
inline const char *Scopes = "https://www.googleapis.com/auth/calendar.app.created "
                            "https://www.googleapis.com/auth/tasks";

struct Endpoints {
    QString auth = "https://accounts.google.com/o/oauth2/v2/auth";
    QString token = "https://oauth2.googleapis.com/token";
    QString revoke = "https://oauth2.googleapis.com/revoke";
    QString calendar = "https://www.googleapis.com/calendar/v3";
    QString tasks = "https://tasks.googleapis.com/tasks/v1";
    // OMNI_REMINDER_GOOGLE_BASE points everything at one server (the tests'
    // fake Google).
    static Endpoints fromEnvironment();
};

// The "Desktop app" OAuth client from Google Cloud, as downloaded.
struct Client {
    QString id, secret;
    static QString configDirectory();
    static QString defaultPath();
    bool load(const QString &path, QString *error);
};

// The refresh token. In the desktop's Secret Service (GNOME Keyring, through
// secret-tool) when one answers, else a 0600 file beside the client file.
class TokenStore {
public:
    // mode: "keyring", "file", or empty for keyring-then-file.
    // OMNI_REMINDER_TOKEN_STORE sets the mode when none is given.
    explicit TokenStore(const QString &mode = {});
    bool save(const QString &refreshToken, QString *error);
    QString load() const;
    void clear();
    QString location() const; // where the token is now, for `sync status`
    static QString filePath();

private:
    bool keyringAllowed() const;
    QString m_mode;
};

struct Response {
    int status = 0;          // HTTP status, 0 when the request never got an answer
    QByteArray body;
    QString networkError;    // set when status is 0
    QJsonObject json() const;
    bool ok() const { return status >= 200 && status < 300; }
};

// One blocking HTTP request with a time limit.
Response send(QNetworkAccessManager &network, const QByteArray &method, const QUrl &url,
              const QByteArray &body = {}, const QByteArray &contentType = {},
              const QByteArray &bearer = {}, int timeoutMs = 30000);

struct Tokens {
    QString access, refresh;
    QString scope; // what the user actually granted (space separated)
    QDateTime expires;
    bool invalidGrant = false; // the refresh token is dead: sign in again
    bool offline = false;      // no answer: try again later
    QString error;
    bool ok() const { return !access.isEmpty(); }
};

Tokens refresh(QNetworkAccessManager &network, const Endpoints &endpoints, const Client &client,
               const QString &refreshToken);

// PKCE (RFC 7636): a random verifier and its S256 challenge.
QString makeVerifier();
QString challengeFor(const QString &verifier);

// The installed-app flow: listen on 127.0.0.1 at a free port, open the
// consent page, take the code from the redirect, trade it for tokens.
// `open` shows the URL to the user (xdg-open in real use); `say` prints progress.
Tokens login(const Endpoints &endpoints, const Client &client, const std::function<bool(const QUrl &)> &open,
             const std::function<void(const QString &)> &say, int timeoutMs = 5 * 60 * 1000);

void revoke(QNetworkAccessManager &network, const Endpoints &endpoints, const QString &token);

} // namespace Google
