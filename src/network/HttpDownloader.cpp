#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <base64.h>
#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
#endif
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include "AppVersion.h"
#include "network/DownloadFileSwap.h"
#include "network/HttpRedirectPolicy.h"
#include "network/WifiPowerSaveGuard.h"
#include "util/UrlUtils.h"

namespace {
constexpr size_t PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr uint32_t PROGRESS_UPDATE_MS = 250;
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr int HTTP_READ_POLL_TIMEOUT_MS = 5000;
constexpr uint32_t DOWNLOAD_IDLE_TIMEOUT_MS = 30000;
constexpr size_t DEFAULT_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr uint8_t MAX_REDIRECTS = 5;

// esp_http_client builds the request line ("GET <path>?<query> HTTP/1.1") inside its TX buffer and
// fails the whole request with "Out of buffer" when that line does not fit. Lingua's Google engine
// sends the paragraph in the query string, so a fixed 1 KB buffer silently failed every paragraph
// longer than roughly 500 characters. Size the buffer to the URL instead (small URLs keep 1 KB).
int txBufferSizeFor(const std::string& url) {
  const size_t needed = url.size() + 64;  // method, " HTTP/1.1\r\n", slack
  return needed > static_cast<size_t>(HTTP_TX_BUF) ? static_cast<int>(needed) : HTTP_TX_BUF;
}

void logNetworkState(const char* phase) {
  LOG_DBG("HTTP", "%s: heap free=%u maxAlloc=%u wifi=%d rssi=%d", phase, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<int>(WiFi.status()), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
}

void logDownloadState(const char* phase, const size_t downloaded, const size_t total, const uint32_t idleMs) {
  LOG_ERR("HTTP", "%s after %zu/%zu bytes (idle=%lu ms, timeout=%lu ms)", phase, downloaded, total,
          static_cast<unsigned long>(idleMs), static_cast<unsigned long>(DOWNLOAD_IDLE_TIMEOUT_MS));
  logNetworkState(phase);
}

bool isRedirect(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t captureLocationHeader(esp_http_client_event_t* evt) {
  auto* location = static_cast<std::string*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && location != nullptr && evt->header_key != nullptr &&
      evt->header_value != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
    location->assign(evt->header_value);
  }
  return ESP_OK;
}

bool isCancelRequested(bool* cancelFlag, const HttpDownloader::CancelCallback& shouldCancel) {
  if (cancelFlag && *cancelFlag) return true;
  if (shouldCancel && shouldCancel()) {
    if (cancelFlag) *cancelFlag = true;
    return true;
  }
  return false;
}

class ProgressNotifier {
 public:
  explicit ProgressNotifier(const HttpDownloader::ProgressCallback& progress) : progress_(&progress) {}

  void setTotal(const size_t total) { total_ = total; }

  void notify(size_t downloaded, bool force) {
    // Known gap: with no Content-Length (chunked or close-delimited bodies)
    // total_ stays 0 and callers never hear about progress, so the OPDS
    // download screen sits at 0% until the transfer ends. Cancel still works
    // through shouldCancel. If a server ever does this for books, report
    // bytes on a timer with total 0 and draw an indeterminate bar.
    if (!progress_ || !*progress_ || total_ == 0) return;

    const uint32_t now = millis();
    if (force || downloaded == total_ || downloaded - lastProgressBytes_ >= PROGRESS_UPDATE_BYTES ||
        now - lastProgressMs_ >= PROGRESS_UPDATE_MS) {
      lastProgressBytes_ = downloaded;
      lastProgressMs_ = now;
      (*progress_)(downloaded, total_);
    }
  }

 private:
  size_t total_ = 0;
  size_t lastProgressBytes_ = 0;
  uint32_t lastProgressMs_ = 0;
  const HttpDownloader::ProgressCallback* progress_ = nullptr;
};

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  HttpDownloader::CancelCallback shouldCancel;
  size_t resumeOffset = 0;
  size_t downloaded = 0;
  size_t total = 0;
  bool rangeIgnored = false;
};

void setRequestHeaders(esp_http_client_handle_t client, const std::string& username, const std::string& password,
                       size_t resumeOffset, bool sendAuthorization) {
  esp_http_client_set_header(client, "User-Agent", AppVersion::userAgent());
  esp_http_client_set_header(client, "Connection", "close");
  if (resumeOffset > 0) {
    char rangeHeader[40];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", resumeOffset);
    esp_http_client_set_header(client, "Range", rangeHeader);
    LOG_DBG("HTTP", "Resuming download at byte %zu", resumeOffset);
  }
  if (sendAuthorization) {
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
}

void logTlsError(esp_http_client_handle_t client, const char* phase) {
  int tlsError = 0;
  int tlsFlags = 0;
  const esp_err_t err = esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
  if (err != ESP_OK || tlsError != 0 || tlsFlags != 0) {
    const int tlsCode = tlsError < 0 ? -tlsError : tlsError;
    LOG_ERR("HTTP", "%s TLS error: err=%s mbedtls=0x%x flags=0x%x", phase, esp_err_to_name(err), tlsCode, tlsFlags);
  }
}

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolfSsl(const std::string& url, const std::string& username,
                                            const std::string& password,
                                            const HttpRedirectPolicy::Url& credentialOrigin, const bool hasCredentials,
                                            Sink& sink, const size_t bufferSize) {
  (void)bufferSize;  // SecureHttpClient owns one fixed 1024-byte streaming buffer.
  std::string currentUrl = url;
  ProgressNotifier progressNotifier(sink.progress);

  for (uint8_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
    HttpRedirectPolicy::Url currentOrigin;
    const bool currentParsed = HttpRedirectPolicy::parseUrl(currentUrl, currentOrigin);
    const bool sendAuthorization =
        currentParsed && HttpRedirectPolicy::shouldSendAuthorization(currentOrigin, credentialOrigin, hasCredentials);

    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    // SecureNet does not yet expose ESP-IDF's CA bundle. This matches the
    // existing KOSync transport; cross-origin hops omit Basic credentials.
    http.setInsecure();
    if (!http.begin(currentUrl)) {
      LOG_ERR("HTTP", "wolfSSL rejected URL: %s", UrlUtils::forLog(currentUrl).c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    // Replace SecureHttpClient's built-in User-Agent so strict servers receive
    // exactly one header while retaining CrossInk's device/version identity.
    http.setUserAgent(AppVersion::userAgent());
    if (sink.resumeOffset > 0) {
      char rangeHeader[40];
      snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", sink.resumeOffset);
      http.addHeader("Range", rangeHeader);
      LOG_DBG("HTTP", "Resuming download at byte %zu", sink.resumeOffset);
    }
    if (sendAuthorization) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }

    LOG_DBG("HTTP", "wolfSSL GET: %s", UrlUtils::forLog(currentUrl).c_str());
    const int status = http.GET(
        [&http, &sink, &progressNotifier](const uint8_t* data, const size_t len) {
          const int responseStatus = http.getStatus();
          const bool isResumeResponse = sink.resumeOffset > 0 && responseStatus == 206;
          if (responseStatus != 200 && !isResumeResponse) return true;
          if (sink.resumeOffset > 0 && !isResumeResponse) {
            sink.rangeIgnored = true;
            return false;
          }

          if (sink.downloaded < sink.resumeOffset) sink.downloaded = sink.resumeOffset;
          if (sink.total == 0 && http.hasContentLength()) {
            sink.total = sink.resumeOffset + http.getContentLength();
            progressNotifier.setTotal(sink.total);
          }
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          progressNotifier.notify(sink.downloaded, false);
          return true;
        },
        [&sink]() { return isCancelRequested(sink.cancelFlag, sink.shouldCancel); });

    if (http.aborted()) return HttpDownloader::ABORTED;
    if (sink.rangeIgnored) {
      LOG_DBG("HTTP", "Server ignored range request; restarting download");
      sink.resumeOffset = 0;
      return HttpDownloader::HTTP_ERROR;
    }
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", UrlUtils::forLog(currentUrl).c_str());
      logNetworkState("wolfSSL request failure");
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty()) {
        LOG_ERR("HTTP", "Redirect missing Location header");
        return HttpDownloader::HTTP_ERROR;
      }

      const std::string redirectUrl = HttpRedirectPolicy::buildRedirectUrl(currentUrl, location);
      HttpRedirectPolicy::Url redirect;
      if (!HttpRedirectPolicy::parseUrl(redirectUrl, redirect)) {
        LOG_ERR("HTTP", "Rejected redirect with unsupported Location");
        return HttpDownloader::HTTP_ERROR;
      }
      if (currentParsed && !HttpRedirectPolicy::isAllowedRedirect(currentOrigin, redirect)) {
        LOG_ERR("HTTP", "Rejected HTTPS downgrade redirect to %s", redirect.host.c_str());
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = redirectUrl;
      LOG_DBG("HTTP", "Redirecting to: %s", redirect.host.c_str());
      continue;
    }

    const bool isResumeResponse = sink.resumeOffset > 0 && status == 206;
    if (status != 200 && !isResumeResponse) {
      LOG_ERR("HTTP", "Unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) {
      LOG_ERR("HTTP", "Write failed after %zu/%zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::FILE_ERROR;
    }
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "Incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::HTTP_ERROR;
    }

    if (sink.total == 0 && http.hasContentLength()) {
      sink.total = sink.resumeOffset + http.getContentLength();
      progressNotifier.setTotal(sink.total);
    }
    progressNotifier.notify(sink.downloaded, true);
    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "Redirect limit exceeded");
  return HttpDownloader::HTTP_ERROR;
}
#endif

HttpDownloader::DownloadError runGetDefault(const std::string& url, const std::string& username,
                                            const std::string& password,
                                            const HttpRedirectPolicy::Url& credentialOrigin, const bool hasCredentials,
                                            Sink& sink, const size_t bufferSize) {
  std::string currentUrl = url;

  for (uint8_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
    HttpRedirectPolicy::Url currentOrigin;
    const bool currentParsed = HttpRedirectPolicy::parseUrl(currentUrl, currentOrigin);
    const bool sendAuthorization =
        currentParsed && HttpRedirectPolicy::shouldSendAuthorization(currentOrigin, credentialOrigin, hasCredentials);
    std::string redirectLocation;

    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.buffer_size = HTTP_RX_BUF;
    config.buffer_size_tx = txBufferSizeFor(currentUrl);
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;
    config.event_handler = captureLocationHeader;
    config.user_data = &redirectLocation;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "Client init failed");
      logNetworkState("Client init failure");
      return HttpDownloader::HTTP_ERROR;
    }

    setRequestHeaders(client, username, password, sink.resumeOffset, sendAuthorization);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Open failed: %s", esp_err_to_name(err));
      logTlsError(client, "Open failure");
      logNetworkState("Open failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    int64_t responseLength = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (responseLength < 0) {
      LOG_ERR("HTTP", "Fetch headers failed: %lld", static_cast<long long>(responseLength));
      logNetworkState("Fetch headers failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      if (redirectLocation.empty()) {
        LOG_ERR("HTTP", "Redirect missing Location header");
        logNetworkState("Redirect missing Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }

      const std::string redirectUrl = HttpRedirectPolicy::buildRedirectUrl(currentUrl, redirectLocation);
      HttpRedirectPolicy::Url redirect;
      if (!HttpRedirectPolicy::parseUrl(redirectUrl, redirect)) {
        LOG_ERR("HTTP", "Rejected redirect with unsupported Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (currentParsed && !HttpRedirectPolicy::isAllowedRedirect(currentOrigin, redirect)) {
        LOG_ERR("HTTP", "Rejected HTTPS downgrade redirect to %s", redirect.host.c_str());
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = redirectUrl;
      LOG_DBG("HTTP", "Redirecting to: %s", redirect.host.c_str());
      esp_http_client_cleanup(client);
      continue;
    }

    const bool isResumeResponse = sink.resumeOffset > 0 && status == 206;
    if (status != 200 && !isResumeResponse) {
      LOG_ERR("HTTP", "Unexpected status: %d", status);
      logNetworkState("Unexpected status");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (sink.resumeOffset > 0 && !isResumeResponse) {
      LOG_DBG("HTTP", "Server ignored range request; restarting download");
      sink.rangeIgnored = true;
      sink.resumeOffset = 0;
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    const size_t bodyLength = responseLength > 0 ? static_cast<size_t>(responseLength) : 0;
    sink.total = bodyLength > 0 ? sink.resumeOffset + bodyLength : 0;
    sink.downloaded = sink.resumeOffset;
    if (sink.total > 0) {
    } else {
    }
#ifdef ESP_ERR_HTTP_EAGAIN
    err = esp_http_client_set_timeout_ms(client, HTTP_READ_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Failed to set read timeout: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
#endif

    auto buffer = makeUniqueNoThrow<char[]>(bufferSize);
    if (!buffer) {
      LOG_ERR("HTTP", "Failed to allocate %zu byte download buffer", bufferSize);
      logNetworkState("Download buffer allocation failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    ProgressNotifier progressNotifier(sink.progress);
    progressNotifier.setTotal(sink.total);
#ifdef ESP_ERR_HTTP_EAGAIN
    uint32_t lastReadMs = millis();
#endif
    while (true) {
      if (isCancelRequested(sink.cancelFlag, sink.shouldCancel)) {
        esp_http_client_cleanup(client);
        return HttpDownloader::ABORTED;
      }

      const int bytesRead = esp_http_client_read(client, buffer.get(), bufferSize);
      if (bytesRead < 0) {
#ifdef ESP_ERR_HTTP_EAGAIN
        if (bytesRead == -ESP_ERR_HTTP_EAGAIN) {
          const uint32_t idleMs = millis() - lastReadMs;
          if (idleMs >= DOWNLOAD_IDLE_TIMEOUT_MS) {
            logDownloadState("Read timed out", sink.downloaded, sink.total, idleMs);
            esp_http_client_cleanup(client);
            return HttpDownloader::HTTP_ERROR;
          }
          delay(1);
          continue;
        }
#endif
        LOG_ERR("HTTP", "Read error after %zu/%zu bytes", sink.downloaded, sink.total);
        logNetworkState("Read error");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (bytesRead == 0) break;

      if (!sink.write(reinterpret_cast<const uint8_t*>(buffer.get()), static_cast<size_t>(bytesRead))) {
        LOG_ERR("HTTP", "Write failed after %zu/%zu bytes", sink.downloaded, sink.total);
        logNetworkState("Write failure");
        esp_http_client_cleanup(client);
        return HttpDownloader::FILE_ERROR;
      }

      sink.downloaded += static_cast<size_t>(bytesRead);
#ifdef ESP_ERR_HTTP_EAGAIN
      lastReadMs = millis();
#endif
      if (sink.total > 0 && sink.total <= PROGRESS_UPDATE_BYTES) {
      }
      progressNotifier.notify(sink.downloaded, false);
      if (sink.total > 0 && sink.downloaded >= sink.total) break;
      delay(0);
    }

    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    progressNotifier.notify(sink.downloaded, true);
    if (!complete) {
      LOG_ERR("HTTP", "Incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      logNetworkState("Incomplete transfer");
      return HttpDownloader::HTTP_ERROR;
    }

    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "Redirect limit exceeded");
  logNetworkState("Redirect limit exceeded");
  return HttpDownloader::HTTP_ERROR;
}

HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     const std::string_view authorizationOrigin, Sink& sink, const size_t bufferSize,
                                     const HttpDownloader::Transport transport) {
  HttpRedirectPolicy::Url credentialOrigin;
  const std::string_view credentialUrl = authorizationOrigin.empty() ? std::string_view(url) : authorizationOrigin;
  const bool hasCredentials =
      !username.empty() && !password.empty() && HttpRedirectPolicy::parseUrl(credentialUrl, credentialOrigin);
#if defined(FREEINK_NET_WOLFSSL)
  if (transport == HttpDownloader::Transport::WOLFSSL) {
    return runGetWolfSsl(url, username, password, credentialOrigin, hasCredentials, sink, bufferSize);
  }
#else
  (void)transport;
#endif
  return runGetDefault(url, username, password, credentialOrigin, hasCredentials, sink, bufferSize);
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password);
}

int HttpDownloader::lastHttpCode = 0;
const char* HttpDownloader::lastErrorName = nullptr;

namespace {
// Lingua: buffered request (translation engines answer in the KB range). Uses perform() with an
// ON_DATA handler, which follows redirects and works on both the device and the simulator.
esp_err_t collectResponseBody(esp_http_client_event_t* evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr && evt->data != nullptr &&
      evt->data_len > 0) {
    static_cast<std::string*>(evt->user_data)->append(static_cast<const char*>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

bool runBufferedRequest(const bool isPost, const std::string& url, const std::string& body, const char* contentType,
                        const char* extraHeaderName, const char* extraHeaderValue, const char* userAgent,
                        std::string& outContent, int& outStatus, const char** outErrorName = nullptr) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;
  outContent.clear();
  outStatus = -1;
  if (outErrorName) *outErrorName = nullptr;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = isPost ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = txBufferSizeFor(url);
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  config.event_handler = collectResponseBody;
  config.user_data = &outContent;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "Buffered request client init failed");
    logNetworkState("Buffered client init failure");
    if (outErrorName) *outErrorName = "client init failed";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", userAgent ? userAgent : AppVersion::userAgent());
  if (contentType && *contentType) {
    esp_http_client_set_header(client, "Content-Type", contentType);
  }
  if (extraHeaderName && extraHeaderValue) {
    esp_http_client_set_header(client, extraHeaderName, extraHeaderValue);
  }
  if (isPost && !body.empty()) {
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
  }

  const esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    // A 401 whose WWW-Authenticate scheme esp_http_client cannot answer (Google sends "Bearer" for a
    // wrong key) makes perform() fail with ESP_ERR_NOT_SUPPORTED even though a complete HTTP
    // response arrived. Report that status like any other so callers can say "invalid key".
    const int status = esp_http_client_get_status_code(client);
    if (status >= 400) {
      LOG_ERR("HTTP", "Buffered request status %d (%s, %u byte body)", status, esp_err_to_name(err),
              static_cast<unsigned>(outContent.size()));
      outStatus = status;
      esp_http_client_cleanup(client);
      return false;
    }
    LOG_ERR("HTTP", "Buffered request failed: %s", esp_err_to_name(err));
    logTlsError(client, "Buffered request");
    logNetworkState("Buffered request failure");
    if (outErrorName) *outErrorName = esp_err_to_name(err);
    esp_http_client_cleanup(client);
    return false;
  }
  outStatus = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (outStatus != 200) {
    LOG_ERR("HTTP", "Buffered request status %d (%u byte body)", outStatus, static_cast<unsigned>(outContent.size()));
    return false;
  }
  return true;
}
}  // namespace

bool HttpDownloader::post(const std::string& url, const std::string& body, const char* contentType,
                          const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent) {
  LOG_DBG("HTTP", "POST (body=%u bytes)", static_cast<unsigned>(body.size()));
  int status = -1;
  const bool ok =
      runBufferedRequest(true, url, body, contentType, extraHeaderName, extraHeaderValue, nullptr, outContent, status,
                         &lastErrorName);
  lastHttpCode = status;
  return ok;
}

bool HttpDownloader::postFile(const std::string& url, const std::string& bodyPath, const char* contentType,
                              const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent,
                              const int timeoutMs) {
  lastHttpCode = -1;
  lastErrorName = nullptr;
  outContent.clear();

  HalFile file;
  if (!Storage.openFileForRead("HTTP", bodyPath, file)) {
    lastErrorName = "body file";
    return false;
  }
  const size_t bodySize = file.size();
  LOG_DBG("HTTP", "POST file (body=%u bytes)", static_cast<unsigned>(bodySize));

#if defined(SIMULATOR)
  // The host shim has no esp_http_client_write(); the host has RAM to spare, so send it buffered.
  std::string body(bodySize, '\0');
  const int readBytes = bodySize > 0 ? file.read(body.data(), bodySize) : 0;
  file.close();
  if (readBytes < 0 || static_cast<size_t>(readBytes) != bodySize) {
    lastErrorName = "body file";
    return false;
  }
  (void)timeoutMs;
  return post(url, body, contentType, extraHeaderName, extraHeaderValue, outContent);
#else
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = txBufferSizeFor(url);
  config.timeout_ms = timeoutMs;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    file.close();
    logNetworkState("File POST client init failure");
    lastErrorName = "client init failed";
    return false;
  }
  esp_http_client_set_header(client, "User-Agent", AppVersion::userAgent());
  if (contentType && *contentType) esp_http_client_set_header(client, "Content-Type", contentType);
  if (extraHeaderName && extraHeaderValue) esp_http_client_set_header(client, extraHeaderName, extraHeaderValue);

  esp_err_t err = esp_http_client_open(client, static_cast<int>(bodySize));
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "File POST open failed: %s", esp_err_to_name(err));
    logTlsError(client, "File POST open");
    logNetworkState("File POST open failure");
    lastErrorName = esp_err_to_name(err);
    esp_http_client_cleanup(client);
    file.close();
    return false;
  }

  constexpr size_t CHUNK = 2048;
  char chunk[CHUNK];
  size_t sent = 0;
  while (sent < bodySize) {
    const int n = file.read(chunk, std::min(CHUNK, bodySize - sent));
    if (n <= 0) break;
    int offset = 0;
    while (offset < n) {
      const int w = esp_http_client_write(client, chunk + offset, n - offset);
      if (w <= 0) break;
      offset += w;
    }
    if (offset < n) break;
    sent += static_cast<size_t>(n);
  }
  file.close();
  if (sent != bodySize) {
    LOG_ERR("HTTP", "File POST write stopped at %u/%u bytes", static_cast<unsigned>(sent),
            static_cast<unsigned>(bodySize));
    logNetworkState("File POST write failure");
    lastErrorName = "upload interrupted";
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  if (esp_http_client_fetch_headers(client) < 0) {
    LOG_ERR("HTTP", "File POST: no response headers");
    lastErrorName = "no response";
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }
  const int status = esp_http_client_get_status_code(client);
  char rx[512];
  while (true) {
    const int n = esp_http_client_read(client, rx, sizeof(rx));
    if (n <= 0) break;
    outContent.append(rx, static_cast<size_t>(n));
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  lastHttpCode = status;
  if (status != 200) {
    LOG_ERR("HTTP", "File POST status %d (%u byte body)", status, static_cast<unsigned>(outContent.size()));
    return false;
  }
  return true;
#endif
}

bool HttpDownloader::postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                              std::string& outContent) {
  const char* authName = authHeader.empty() ? nullptr : "Authorization";
  const char* authValue = authHeader.empty() ? nullptr : authHeader.c_str();
  return post(url, jsonBody, "application/json", authName, authValue, outContent);
}

bool ReusableHttpSession::waitForHeapReady(const uint32_t timeoutMs, volatile const bool* cancelFlag) {
  const uint32_t start = millis();
  while (ESP.getFreeHeap() < HttpDownloader::MIN_FREE_HEAP_FOR_TLS ||
         ESP.getMaxAllocHeap() < HttpDownloader::MIN_MAX_ALLOC_FOR_TLS) {
    if (cancelFlag && *cancelFlag) return false;
    if (millis() - start >= timeoutMs) {
      LOG_ERR("HTTP", "Heap not ready for TLS after %lu ms (free=%u maxAlloc=%u)", static_cast<unsigned long>(timeoutMs),
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return false;
    }
    delay(50);
  }
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, const char* userAgent) {
  if (userAgent != nullptr && username.empty() && password.empty()) {
    int status = -1;
    return runBufferedRequest(false, url, std::string(), nullptr, nullptr, nullptr, userAgent, outContent, status);
  }
  outContent.clear();
  return fetchUrl(
      url,
      [&outContent](const uint8_t* data, size_t len) {
        outContent.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      username, password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  return streamUrl(url, onData, nullptr, username, password) == OK;
}

HttpDownloader::DownloadError HttpDownloader::streamUrl(const std::string& url, const DataCallback& onData,
                                                        ProgressCallback progress, const std::string& username,
                                                        const std::string& password, DownloadOptions options) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  if (!onData) {
    LOG_ERR("HTTP", "Fetch failed: missing data callback");
    return HTTP_ERROR;
  }

  Sink sink;
  sink.write = onData;
  sink.progress = std::move(progress);
  sink.shouldCancel = std::move(options.shouldCancel);
  const size_t bufferSize = options.bufferSize > 0 ? options.bufferSize : DEFAULT_DOWNLOAD_BUFFER_SIZE;
  return runGet(url, username, password, options.authorizationOrigin, sink, bufferSize, options.transport);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             DownloadOptions options) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  const size_t bufferSize = options.bufferSize > 0 ? options.bufferSize : DEFAULT_DOWNLOAD_BUFFER_SIZE;
  if (options.stageAsPart && !DownloadFileSwap::recover(destPath)) return FILE_ERROR;
  const std::string writePath = options.stageAsPart ? destPath + ".part" : destPath;
  size_t resumeOffset = 0;
  if (options.resumePartial && Storage.exists(writePath.c_str())) {
    FsFile existingFile;
    if (Storage.openFileForRead("HTTP", writePath.c_str(), existingFile)) {
      resumeOffset = existingFile.fileSize();
      existingFile.close();
    }
  }

  if (resumeOffset == 0 && Storage.exists(writePath.c_str())) {
    Storage.remove(writePath.c_str());
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.shouldCancel = std::move(options.shouldCancel);
  sink.resumeOffset = resumeOffset;

  FsFile file;
  bool fileOpen = false;
#ifndef SIMULATOR
  bool spaceChecked = false;
#endif
  bool insufficientSpace = false;
  auto openOutputFile = [&]() {
    if (fileOpen) return true;
#ifndef SIMULATOR
    // The host storage shim does not expose card capacity.
    if (options.checkFreeSpace && !spaceChecked) {
      spaceChecked = true;
      // Some SD transports cannot report capacity; let the write fail instead.
      const uint64_t totalBytes = Storage.totalBytes();
      if (totalBytes > 0 && sink.total > sink.resumeOffset) {
        const uint64_t usedBytes = Storage.usedBytes();
        const uint64_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
        const uint64_t neededBytes = sink.total - sink.resumeOffset;
        if (freeBytes < neededBytes) {
          LOG_ERR("HTTP", "Insufficient SD space: free=%llu required=%llu", static_cast<unsigned long long>(freeBytes),
                  static_cast<unsigned long long>(neededBytes));
          insufficientSpace = true;
          return false;
        }
      }
    }
#endif
    if (sink.resumeOffset > 0) {
      file = Storage.open(writePath.c_str(), O_WRONLY | O_APPEND);
    } else {
      fileOpen = Storage.openFileForWrite("HTTP", writePath.c_str(), file);
      if (!fileOpen) {
        LOG_ERR("HTTP", "Failed to open file for writing");
        return false;
      }
    }
    fileOpen = file;
    if (!fileOpen) {
      LOG_ERR("HTTP", "Failed to open file for writing");
    }
    return fileOpen;
  };

  sink.write = [&](const uint8_t* data, size_t len) { return openOutputFile() && file.write(data, len) == len; };

  DownloadError result =
      runGet(url, username, password, options.authorizationOrigin, sink, bufferSize, options.transport);
  if (sink.rangeIgnored) {
    if (fileOpen) {
      file.close();
      fileOpen = false;
    }
    Storage.remove(writePath.c_str());
    sink.rangeIgnored = false;
    sink.resumeOffset = 0;
    sink.downloaded = 0;
    sink.total = 0;
    sink.write = [&](const uint8_t* data, size_t len) { return openOutputFile() && file.write(data, len) == len; };
    result = runGet(url, username, password, options.authorizationOrigin, sink, bufferSize, options.transport);
  }

  if (fileOpen) {
    const bool synced = file.sync();
    const bool closed = file.close();
    if (!synced || !closed) {
      LOG_ERR("HTTP", "Failed to finish downloaded file");
      result = FILE_ERROR;
    }
  }
  if (insufficientSpace) result = INSUFFICIENT_SPACE;

  if (result != OK) {
    LOG_ERR("HTTP", "Transfer failed: error=%d downloaded=%zu expected=%zu preservePartial=%d resumePartial=%d",
            static_cast<int>(result), sink.downloaded, sink.total, options.preservePartial, options.resumePartial);
    if (result == ABORTED || !options.preservePartial) {
      Storage.remove(writePath.c_str());
    }
    return result;
  }

  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    if (!options.preservePartial) {
      Storage.remove(writePath.c_str());
    }
    return HTTP_ERROR;
  }

  if (sink.total > 0 && sink.downloaded != sink.total) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", sink.downloaded, sink.total);
    if (!options.preservePartial) {
      Storage.remove(writePath.c_str());
    }
    return HTTP_ERROR;
  }

  if (options.validate && !options.validate(writePath)) {
    LOG_ERR("HTTP", "Downloaded file failed validation: %s", writePath.c_str());
    Storage.remove(writePath.c_str());
    return HTTP_ERROR;
  }

  if (options.stageAsPart && !DownloadFileSwap::publish(destPath)) return FILE_ERROR;

  return OK;
}
