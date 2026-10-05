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
 * Lingua: a burst of requests to the same origin (one chapter's translation batches). On this
 * firmware every request delegates to the matching HttpDownloader static (one connection per
 * request), so behavior is identical to not using a session; the class keeps the translator's
 * interface and its heap backpressure.
 */
class ReusableHttpSession {
 public:
  ReusableHttpSession() = default;
  ~ReusableHttpSession() = default;
  ReusableHttpSession(const ReusableHttpSession&) = delete;
  ReusableHttpSession& operator=(const ReusableHttpSession&) = delete;

  bool fetchUrl(const std::string& url, std::string& outContent) { return HttpDownloader::fetchUrl(url, outContent); }
  bool post(const std::string& url, const std::string& body, const char* contentType, const char* extraHeaderName,
            const char* extraHeaderValue, std::string& outContent) {
    return HttpDownloader::post(url, body, contentType, extraHeaderName, extraHeaderValue, outContent);
  }
  bool postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                std::string& outContent) {
    return HttpDownloader::postJson(url, jsonBody, authHeader, outContent);
  }

  // Heap backpressure before a request's TLS connect: waits (feeding the watchdog) until the heap
  // can support a handshake, or until timeoutMs elapses / *cancelFlag is set. Returns false only on
  // a genuine timeout or cancel.
  bool waitForHeapReady(uint32_t timeoutMs, volatile const bool* cancelFlag);
};
