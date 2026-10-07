#pragma once
#include <HalStorage.h>
#include <Stream.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

/**
 * HTTP client utility for fetching content and downloading files.
 * Streams requests through the configured HTTP transport so large downloads
 * do not need to fit in RAM.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  using CancelCallback = std::function<bool()>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    INSUFFICIENT_SPACE,
  };

  enum class Transport {
    ESP_HTTP,
    WOLFSSL,
  };

  struct DownloadOptions {
    explicit DownloadOptions(bool preservePartial = false, bool resumePartial = false,
                             CancelCallback shouldCancel = nullptr, size_t bufferSize = 0,
                             Transport transport = Transport::ESP_HTTP)
        : preservePartial(preservePartial),
          resumePartial(resumePartial),
          shouldCancel(std::move(shouldCancel)),
          bufferSize(bufferSize),
          transport(transport) {}

    bool preservePartial;
    bool resumePartial;
    CancelCallback shouldCancel;
    size_t bufferSize;
    Transport transport;
    // Borrowed only for this synchronous request. Basic credentials are sent
    // only to this origin; empty keeps the request URL as the credential origin.
    std::string_view authorizationOrigin;
    // Download to "<destPath>.part" and rename it over destPath only once the
    // transfer succeeds, so a failed or interrupted download never replaces
    // (or deletes) an existing file and never leaves a truncated one behind
    // under the real name.
    bool stageAsPart = false;
    // Checks the finished file before it is accepted (and, with stageAsPart,
    // before it replaces destPath). Returning false fails the download and
    // removes the file. Catches bodies cut short without a Content-Length.
    bool (*validate)(const std::string& path) = nullptr;
    // Once the response length is known, fail with INSUFFICIENT_SPACE before
    // writing anything if the SD card cannot hold the file. The first check
    // can scan the whole FAT, so leave it off for small files.
    bool checkFreeSpace = false;
  };

  // Lingua: TLS heap floors the translation activities apply before starting a chapter's requests.
  static constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 45000;
  static constexpr uint32_t MIN_MAX_ALLOC_FOR_TLS = 20000;
  // Heap a batch needs next to an open ReusableHttpSession (paragraph, URL-encoded copy, response).
  static constexpr uint32_t SESSION_MIN_FREE_HEAP = 16000;
  static constexpr uint32_t SESSION_MIN_MAX_ALLOC = 8000;
  static constexpr uint32_t MIN_FREE_HEAP_FOR_REUSE = 12000;

  /**
   * Fetch text content from a URL with optional credentials.
   * @param userAgent Overrides the default User-Agent for this call only (nullptr keeps it). Needed
   *        for UA-sensitive endpoints (the anonymous Azure/Edge token endpoint expects a browser UA).
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", const char* userAgent = nullptr);

  /**
   * Lingua: POST with configurable content type and one extra header; buffers the response body.
   * Returns true only on HTTP 200. Sets lastHttpCode on every exit path.
   */
  static bool post(const std::string& url, const std::string& body, const char* contentType,
                   const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent);

  /** Lingua: POST JSON with an optional Authorization header value (e.g. "Bearer xxx"). */
  static bool postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                       std::string& outContent);

  /**
   * POST the contents of an SD-card file as the request body, streamed in small chunks so a body of
   * hundreds of KB never has to fit in RAM (the book assistant's context). The response body is
   * buffered into outContent (also on errors, for the API's error message). Returns true only on
   * HTTP 200; sets lastHttpCode / lastErrorName like post().
   */
  static bool postFile(const std::string& url, const std::string& bodyPath, const char* contentType,
                       const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent,
                       int timeoutMs = 120000);

  // Last HTTP response code from the most recent post/postJson call. Negative values indicate
  // connection-level failures; positive values are HTTP status codes.
  static int lastHttpCode;
  // Name of the transport error (e.g. "ESP_ERR_HTTP_CONNECT") when the most recent post/postJson
  // failed before any HTTP status arrived; nullptr otherwise. Shown to the user so a failure on the
  // device can be told apart (DNS/connect, TLS, timeout) without a serial log.
  static const char* lastErrorName;

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream a URL with cancellation/progress support and a detailed result.
   */
  static DownloadError streamUrl(const std::string& url, const DataCallback& onData,
                                 ProgressCallback progress = nullptr, const std::string& username = "",
                                 const std::string& password = "", DownloadOptions options = DownloadOptions());

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      DownloadOptions options = DownloadOptions());
};

/**
 * Lingua: a burst of requests to the same origin (one chapter's translation batches). The session
 * keeps one HTTPS connection open across requests, so the TLS handshake - by far the largest heap
 * peak of a request (~55 KB on an X3 with Wi-Fi up, leaving only a few KB) - runs once per origin
 * instead of once per paragraph. Requests follow the matching HttpDownloader statics' contracts
 * (status handling; post/postJson set lastHttpCode/lastErrorName, fetchUrl leaves them alone). A
 * request that fails on a reused connection (the server may have closed it) is retried once on a
 * fresh one. The connection is closed by the destructor. The simulator's HTTP client has no
 * per-request setters, so there every request delegates to the HttpDownloader statics.
 */
class ReusableHttpSession {
 public:
  ReusableHttpSession() = default;
  ~ReusableHttpSession() { close(); }
  ReusableHttpSession(const ReusableHttpSession&) = delete;
  ReusableHttpSession& operator=(const ReusableHttpSession&) = delete;

  bool fetchUrl(const std::string& url, std::string& outContent);
  bool post(const std::string& url, const std::string& body, const char* contentType, const char* extraHeaderName,
            const char* extraHeaderValue, std::string& outContent);
  bool postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                std::string& outContent) {
    const char* authName = authHeader.empty() ? nullptr : "Authorization";
    const char* authValue = authHeader.empty() ? nullptr : authHeader.c_str();
    return post(url, jsonBody, "application/json", authName, authValue, outContent);
  }

  // Heap backpressure before a request's TLS connect: waits (feeding the watchdog) until the heap
  // can support a handshake, or until timeoutMs elapses / *cancelFlag is set. Returns false only on
  // a genuine timeout or cancel.
  bool waitForHeapReady(uint32_t timeoutMs, volatile const bool* cancelFlag);

  // Closes the open connection, freeing its TLS buffers; the next request reconnects.
  void close();

 private:
  bool request(bool isPost, const std::string& url, const std::string& body, const char* contentType,
               const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent, int& outStatus,
               const char** outErrorName);
  bool requestOnce(bool isPost, const std::string& url, const std::string& body, const char* contentType,
                   const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent,
                   int& outStatus, const char** outErrorName, bool& transportFailed);
  bool ensureClient(const std::string& url);

  void* client = nullptr;       // esp_http_client_handle_t of the open connection
  std::string origin;           // scheme://host[:port] the open client is bound to
  int txBufferSize = 0;         // request-line buffer the client was created with
  std::string extraHeader;      // per-request header last set, cleared before the next request
  bool contentTypeSet = false;
};
