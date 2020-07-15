/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QDesktopServices>
#include <QNetworkReply>
#include <QTimer>
#include <QBuffer>
#include "account.h"
#include "creds/oauth.h"
#include <QJsonObject>
#include <QJsonDocument>
#include "theme.h"
#include "networkjobs.h"
#include "creds/httpcredentials.h"
#include <QRandomGenerator>

namespace {
// TODO: make theming parameter
QString SCOPE()
{
    return QStringLiteral("openid offline_access email profile");
}
}

namespace OCC {

Q_LOGGING_CATEGORY(lcOauth, "sync.credentials.oauth", QtInfoMsg)

OAuth::OAuth(Account *account, QObject *parent)
: QObject(parent)
, _account(account)
{
}

OAuth::~OAuth()
{
}

static void httpReplyAndClose(QTcpSocket *socket, const char *code, const char *html,
const char *moreHeaders = nullptr)
{
    if (!socket)
        return; // socket can have been deleted if the browser was closed
    socket->write("HTTP/1.1 ");
    socket->write(code);
    socket->write("\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: ");
    socket->write(QByteArray::number(qstrlen(html)));
    if (moreHeaders) {
        socket->write("\r\n");
        socket->write(moreHeaders);
    }
    socket->write("\r\n\r\n");
    socket->write(html);
    socket->disconnectFromHost();
    // We don't want that deleting the server too early prevent queued data to be sent on this socket.
    // The socket will be deleted after disconnection because disconnected is connected to deleteLater
    socket->setParent(nullptr);
}

void OAuth::startAuthentication()
{
    // Listen on the socket to get a port which will be used in the redirect_uri
    if (!_server.listen(QHostAddress::LocalHost)) {
        emit result(NotSupported, QString());
        return;
    }

    _pkceCodeVerifier = generateRandomString(24);
    ASSERT(_pkceCodeVerifier.size() == 128)
    _state = generateRandomString(8);

    connect(this, &OAuth::fetchWellKnownFinished, this, [this]{
        Q_EMIT authorisationLinkChanged(authorisationLink());
    });
    fetchWellKnown();

    openBrowser();

    QObject::connect(&_server, &QTcpServer::newConnection, this, [this] {
        while (QPointer<QTcpSocket> socket = _server.nextPendingConnection()) {
            QObject::connect(socket.data(), &QTcpSocket::disconnected, socket.data(), &QTcpSocket::deleteLater);
            QObject::connect(socket.data(), &QIODevice::readyRead, this, [this, socket] {
                const QByteArray peek = socket->peek(qMin(socket->bytesAvailable(), 4000LL)); //The code should always be within the first 4K
                if (peek.indexOf('\n') < 0)
                    return; // wait until we find a \n
                if (!peek.startsWith("GET /?")) {
                    httpReplyAndClose(socket, "404 Not Found", "<html><head><title>404 Not Found</title></head><body><center><h1>404 Not Found</h1></center></body></html>");
                    return;
                }
                const int offset = 6;
                const QUrlQuery args(peek.mid(offset, peek.indexOf(' ', offset) - offset));
                if (args.queryItemValue(QStringLiteral("state")) != _state) {
                    httpReplyAndClose(socket, "400 Bad Request", "<html><head><title>400 Bad Request</title></head><body><center><h1>400 Bad Request</h1></center></body></html>");
                    return;
                }
              auto job = postTokenRequest({
                    { QStringLiteral("grant_type"), "authorization_code" },
                    { QStringLiteral("code"), args.queryItemValue(QStringLiteral("code")) },
                    { QStringLiteral("redirect_uri"), QStringLiteral("http://localhost:%1").arg(_server.serverPort()) },
                    { QStringLiteral("code_verifier"), _pkceCodeVerifier },
                    });
                QObject::connect(job, &SimpleNetworkJob::finishedSignal, this, [this, socket](QNetworkReply *reply) {
                    const auto jsonData = reply->readAll();
                    QJsonParseError jsonParseError;
                    const QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();
                    QString fieldsError;
                    const QString accessToken = getRequiredField(json, QStringLiteral("access_token"), &fieldsError).toString();
                    const QString refreshToken = getRequiredField(json, QStringLiteral("refresh_token"), &fieldsError).toString();
                    const QString tokenType = getRequiredField(json, QStringLiteral("token_type"), &fieldsError).toString().toLower();
                    const QString user = json[QStringLiteral("user_id")].toString();
                    const QUrl messageUrl = json[QStringLiteral("message_url")].toString();

                    if (reply->error() != QNetworkReply::NoError || jsonParseError.error != QJsonParseError::NoError
                        || !fieldsError.isEmpty()
                        || tokenType != "bearer") {
                        // do we have error message suitable for users?
                        QString errorReason = json[QStringLiteral("error_description")].toString();
                        if (errorReason.isEmpty()) {
                            // fall back to technical error
                            errorReason = json[QStringLiteral("error")].toString();
                        }
                        if (!errorReason.isEmpty()) {
                            errorReason = tr("Error returned from the server: <em>%1</em>")
                                              .arg(errorReason.toHtmlEscaped());
                        } else if (reply->error() != QNetworkReply::NoError) {
                            errorReason = tr("There was an error accessing the 'token' endpoint: <br><em>%1</em>")
                                              .arg(reply->errorString().toHtmlEscaped());
                        } else if (jsonData.isEmpty()) {
                            // Can happen if a funky load balancer strips away POST data, e.g. BigIP APM my.policy
                            errorReason = tr("Empty JSON from OAuth2 redirect");
                            // We explicitly have this as error case since the json qcWarning output below is misleading,
                            // it will show a fake json will null values that actually never was received like this as
                            // soon as you access json["whatever"] the debug output json will claim to have "whatever":null
                        } else if (jsonParseError.error != QJsonParseError::NoError) {
                            errorReason = tr("Could not parse the JSON returned from the server: <br><em>%1</em>")
                                              .arg(jsonParseError.errorString());
                        } else if (tokenType != QStringLiteral("bearer")) {
                            errorReason = tr("Unsupported token type: %1").arg(tokenType);
                        } else if (!fieldsError.isEmpty()) {
                            errorReason = tr("The reply from the server did not contain all expected fields\n:%1").arg(fieldsError);
                        } else {
                            errorReason = tr("Unknown Error");
                        }
                        qCWarning(lcOauth) << "Error when getting the accessToken" << errorReason << "received data:" << jsonData;
                        httpReplyAndClose(socket, "500 Internal Server Error",
                            tr("<h1>Login Error</h1><p>%1</p>").arg(errorReason).toUtf8().constData());
                        emit result(Error);
                        return;
                    }
                    if (!user.isEmpty()) {
                        finalize(socket, accessToken, refreshToken, user, messageUrl);
                        return;
                    }
                    // If the reply don't contains the user id, we must do another call to query it
                    JsonApiJob *job = new JsonApiJob(_account->sharedFromThis(), QLatin1String("ocs/v1.php/cloud/user"), this);
                    job->setTimeout(qMin(30 * 1000ll, job->timeoutMsec()));
                    QNetworkRequest req;
                    // We are not connected yet so we need to handle the authentication manually
                    req.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
                    // We just added the Authorization header, don't let HttpCredentialsAccessManager tamper with it
                    req.setAttribute(HttpCredentials::DontAddCredentialsAttribute, true);
                    job->startWithRequest(req);
                    QObject::connect(job, &JsonApiJob::jsonReceived, this, [=](const QJsonDocument &json) {
                        QString user = json.object().value("ocs").toObject().value("data").toObject().value("id").toString();
                        finalize(socket, accessToken, refreshToken, user, messageUrl);
                    });
                });
            });
        }
    });
}

void OAuth::refreshAuthentication(const QString &refreshToken)
{
    connect(this, &OAuth::fetchWellKnownFinished, this, [this, refreshToken] {
        auto job = postTokenRequest({ { QStringLiteral("grant_type"), QStringLiteral("refresh_token") },
            { QStringLiteral("refresh_token"), refreshToken } });
        connect(job, &SimpleNetworkJob::finishedSignal, this, [this, refreshToken](QNetworkReply *reply) {
            const auto jsonData = reply->readAll();
            QString accessToken;
            QString newRefreshToken = refreshToken;
            QJsonParseError jsonParseError;
            // https://developer.okta.com/docs/reference/api/oidc/#response-properties-2
            const QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();
            const QString error = json.value(QLatin1String("error")).toString();
            if (!error.isEmpty()) {
                if (error == QLatin1String("invalid_grant") ||
                    error == QLatin1String("invalid_request")) {
                    newRefreshToken.clear();
                } else {
                    qCWarning(lcOauth) << tr("Error while refreshing the token: %1 : %2").arg(error, json.value(QLatin1String("error_description")).toString());
                }
            } else if (reply->error() != QNetworkReply::NoError) {
                qCWarning(lcOauth) << tr("Error while refreshing the token: %1 : %2").arg(reply->errorString(), QString::fromUtf8(jsonData));
            } else {
                if (jsonParseError.error != QJsonParseError::NoError || json.isEmpty()) {
                    // Invalid or empty JSON: Network error maybe?
                    qCWarning(lcOauth) << tr("Error while refreshing the token: %1 : %2").arg(jsonParseError.errorString(), QString::fromUtf8(jsonData));
                } else {
                    QString error;
                    accessToken = getRequiredField(json, QStringLiteral("access_token"), &error).toString();
                    if (!error.isEmpty()) {
                        qCWarning(lcOauth) << tr("The reply from the server did not contain all expected fields\n:%1\nReceived data: %2").arg(error, QString::fromUtf8(jsonData));
                    }

                    const auto refresh_token = json.find(QStringLiteral("refresh_token"));
                    if (refresh_token != json.constEnd() )
                    {
                        newRefreshToken = refresh_token.value().toString();
                    }
                }
            }
            Q_EMIT refreshFinished(accessToken, newRefreshToken);
        });
    });
    fetchWellKnown();
}

void OAuth::finalize(QPointer<QTcpSocket> socket, const QString &accessToken,
                     const QString &refreshToken, const QString &user, const QUrl &messageUrl) {
    if (!_account->davUser().isNull() && user != _account->davUser()) {
        // Connected with the wrong user
        qCWarning(lcOauth) << "We expected the user" << _account->davUser() << "but the server answered with user" << user;
        const QString message = tr("<h1>Wrong user</h1>"
                                   "<p>You logged-in with user <em>%1</em>, but must login with user <em>%2</em>.<br>"
                                   "Please log out of %3 in another tab, then <a href='%4'>click here</a> "
                                   "and log in as user %2</p>")
                                    .arg(user, _account->davUser(), Theme::instance()->appNameGUI(),
                                        authorisationLink().toString(QUrl::FullyEncoded));
        httpReplyAndClose(socket, "403 Forbidden", message.toUtf8().constData());
        // We are still listening on the socket so we will get the new connection
        return;
    }
    const char *loginSuccessfullHtml = "<h1>Login Successful</h1><p>You can close this window.</p>";
    if (messageUrl.isValid()) {
        httpReplyAndClose(socket, "303 See Other", loginSuccessfullHtml,
            QByteArray("Location: " + messageUrl.toEncoded()).constData());
    } else {
        httpReplyAndClose(socket, "200 OK", loginSuccessfullHtml);
    }
    emit result(LoggedIn, user, accessToken, refreshToken);
}

SimpleNetworkJob *OAuth::postTokenRequest(const QList<QPair<QString, QString>> &queryItems)
{
    const QUrl requestTokenUrl = _tokenEndpoint.isEmpty() ? Utility::concatUrlPath(_account->url(), QLatin1String("/index.php/apps/oauth2/api/v1/token")) : _tokenEndpoint;
    QNetworkRequest req;
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    const QString basicAuth = QStringLiteral("%1:%2").arg(Theme::instance()->oauthClientId(), Theme::instance()->oauthClientSecret()).toUtf8().toBase64();
    req.setRawHeader("Authorization", QStringLiteral("Basic %1").arg(basicAuth).toUtf8());
    req.setAttribute(HttpCredentials::DontAddCredentialsAttribute, true);

    auto requestBody = new QBuffer;
    QUrlQuery arguments;
    arguments.setQueryItems(QList<QPair<QString, QString>> { { QStringLiteral("client_id"), Theme::instance()->oauthClientId() },
                                { QStringLiteral("client_secret"), Theme::instance()->oauthClientSecret() },
                                { QStringLiteral("scope"), SCOPE() } }
        << queryItems);

    requestBody->setData(arguments.query(QUrl::FullyEncoded).toUtf8());

    auto job = _account->sendRequest("POST", requestTokenUrl, req, requestBody);
    job->setTimeout(qMin(30 * 1000ll, job->timeoutMsec()));
    job->setAuthenticationJob(true);
    return job;
}

QByteArray OAuth::generateRandomString(size_t size) const
{
    // TODO: do we need a varaible size?
    std::vector<quint32> buffer(size, 0);
    QRandomGenerator::global()->fillRange(buffer.data(), static_cast<qsizetype>(size));
    return QByteArray(reinterpret_cast<char *>(buffer.data()), static_cast<int>(size * sizeof(quint32))).toBase64(QByteArray::Base64UrlEncoding);
}

QVariant OAuth::getRequiredField(const QJsonObject &json, const QString &s, QString *error)
{
    const auto out = json.constFind(s);
    if (out == json.constEnd()) {
        error->append(tr("\tError: Missing field %1\n").arg(s));
        return QJsonValue();
    }
    return *out;
}

QUrl OAuth::authorisationLink() const
{
    Q_ASSERT(_server.isListening());
    QUrlQuery query;
    const QByteArray code_challenge = QCryptographicHash::hash(_pkceCodeVerifier, QCryptographicHash::Sha256)
                                          .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    query.setQueryItems({
        { QStringLiteral("response_type"), QStringLiteral("code") },
        { QStringLiteral("client_id"), Theme::instance()->oauthClientId() },
        { QStringLiteral("redirect_uri"), QStringLiteral("http://localhost:%1").arg(QString::number(_server.serverPort())) },
        { QStringLiteral("code_challenge"), QString::fromLatin1(code_challenge) },
        { QStringLiteral("code_challenge_method"), QStringLiteral("S256") },
        { QStringLiteral("scope"), SCOPE() },
        { QStringLiteral("prompt"), QStringLiteral("consent") },
        { QStringLiteral("state"), _state },
    });
    if (!_account->davUser().isNull())
        query.addQueryItem(QStringLiteral("user"), _account->davUser().replace(QLatin1Char('+'), QStringLiteral("%2B"))); // Issue #7762
    const QUrl url = _authEndpoint.isValid()
        ? Utility::concatUrlPath(_authEndpoint, {}, query)
        : Utility::concatUrlPath(_account->url(), QStringLiteral("/index.php/apps/oauth2/authorize"), query);
    return url;
}

void OAuth::authorisationLinkAsync(std::function<void (const QUrl &)> callback) const
{
    if (_wellKnownFinished) {
        callback(authorisationLink());
    } else {
        connect(this, &OAuth::authorisationLinkChanged, callback);
    }
}

void OAuth::fetchWellKnown()
{
    const QPair<QString, QString> urls = Theme::instance()->oauthOverrideAuthUrl();
    if (!urls.first.isNull())
    {
        ASSERT(!urls.second.isNull());
        _authEndpoint = urls.first;
        _tokenEndpoint = urls.second;
        _wellKnownFinished = true;
         Q_EMIT fetchWellKnownFinished();
    }
    else
    {
        QUrl wellKnownUrl = Utility::concatUrlPath(_account->url(), QStringLiteral("/.well-known/openid-configuration"));
        QNetworkRequest req;
        auto job = _account->sendRequest("GET", wellKnownUrl);
        job->setAuthenticationJob(true);
        job->setTimeout(qMin(30 * 1000ll, job->timeoutMsec()));
        QObject::connect(job, &SimpleNetworkJob::finishedSignal, this, [this](QNetworkReply *reply) {
            _wellKnownFinished = true;
            if (reply->error() != QNetworkReply::NoError) {
                // Most likely the file does not exist, default to the normal endpoint
                Q_EMIT fetchWellKnownFinished();
                return;
            }
            const auto jsonData = reply->readAll();
            QJsonParseError jsonParseError;
            const QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();

            if (jsonParseError.error == QJsonParseError::NoError) {
                QString authEp = json["authorization_endpoint"].toString();
                if (!authEp.isEmpty())
                    this->_authEndpoint = authEp;
                QString tokenEp = json["token_endpoint"].toString();
                if (!tokenEp.isEmpty())
                    this->_tokenEndpoint = tokenEp;
            } else if (jsonParseError.error == QJsonParseError::IllegalValue) {
                qCDebug(lcOauth) << ".well-known did not return json, the server most does not support oidc";
            } else {
                qCWarning(lcOauth) << "Json parse error in well-known: " << jsonParseError.errorString();
            }
            Q_EMIT fetchWellKnownFinished();
        });
    }
}

void OAuth::openBrowser()
{
    _account->clearCookieJar(); // #6574
    authorisationLinkAsync([this](const QUrl &link) {
        if (!QDesktopServices::openUrl(link)) {
            qCWarning(lcOauth) << "QDesktopServices::openUrl Failed";
            // We cannot open the browser, then we claim we don't support OAuth.
            emit result(NotSupported, QString());
        }
    });
}

} // namespace OCC
