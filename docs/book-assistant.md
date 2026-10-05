# Book Assistant

**Book assistant** (reader menu) asks Google Gemini about the book you are reading. It can recap
the pages you just read, list the main characters so far, answer a question you type, or tell you
who a name on the page is. Answers are written in the reader's interface language (Settings →
Language), whatever language the book is in. They use only the book up to your page, so they don't
spoil what comes next.

## Using it

Open the reader menu and choose **Book assistant**. On touch devices it is in the **More** tab of
the reader drawer. Then pick one of these:

- **Ask a question**: type a question with the on-screen keyboard, for example "Why did Anna leave
  the village?".
- **Who is...? (select in text)**: select a name or a term on the page, the same way you select
  text for a clipping. Press Confirm on the first word, move to the last word and press Confirm
  again; on touch devices, drag over it. The assistant explains who or what it is and its role so
  far.
- **Main characters**: the main characters you have met so far, who they are and where they stand.
- **Recap: last 3, 5, 10 or 20 pages** or **Recap: chapter so far**: a short summary of what you
  just read in the current chapter.
- **Gemini API key**: set or change the key (see below).

The first time, enter your Gemini API key. The reader then connects to Wi-Fi, asks Gemini and shows
the answer. Page through it with the page/arrow buttons (or swipe/tap on touch devices). After a
question, a "who is" answer or the character list, pressing **Confirm** on the last page lets you
ask another question. **Back** returns to the book on the same page. The reader remembers your last
choice.

## How the assistant knows the book

Gemini can read a whole book, but the reader cannot hold one in memory. So the assistant sends:

- the full text of the current chapter, from its start up to your page (the last 120 KB if the
  chapter is very long), and
- a summary of every earlier chapter.

The chapter summaries are made with Gemini the first time you use the assistant on a book, one
request per chapter, and saved on the SD card in the book's cache folder. A progress screen shows
"Summarizing chapter N of M". It takes a few seconds per chapter, so a book you are halfway through
can take a few minutes the first time. Later questions only summarize the chapters you have
finished since, so they are quick. **Back** cancels the preparation; the summaries already made
are kept for next time. Deleting the book's cache (reader menu → Settings) deletes them too.

Details from early chapters are known only through their summaries, so a very specific question
about something far back can get a vaguer answer than one about the current chapter.

**Recap** works differently: it sends only the pages you chose from the current chapter (at most
about 20 KB, roughly 10-15 pages, cutting the oldest first) and needs no chapter summaries.

## Getting a Gemini API key

1. Go to [Google AI Studio](https://aistudio.google.com/apikey) and sign in with a Google account.
2. Create an API key. The free tier needs no credit card.
3. Put the key on the reader, in one of two ways:
   - **From a computer (easier):** save the key alone in a text file named `gemini-api-key.txt` at
     the root of the SD card (over USB, or with the Wi-Fi file transfer). The next time you open
     **Book assistant**, the reader imports the key and deletes the file.
   - **On the device:** choose **Book assistant → Gemini API key** and type it. The key is shown
     while you type, so you can check it: a single wrong character makes Google reject it.

   You can change the key at any time the same ways.

If you already use **Gemini** as the Lingua translation engine, the assistant uses that key until
you set a separate one.

## Models and limits

Answers use `gemini-3-flash-preview` first. If that model is not available for your key, is
rate-limited or is overloaded, the reader falls back to `gemini-3.1-flash-lite`, then
`gemini-flash-latest`. Chapter summaries go the other way: Flash-Lite first, keeping Flash's quota
for your answers. Each model has its own free-tier quota.

The reader waits at least 4 seconds between chapter summaries. If every model is over its
per-minute limit, it waits 30 seconds and tries again, a few times. Google does not publish fixed
free-tier limits: AI Studio shows the current limits for your project, and they can change at any
time. There is also a daily limit, so preparing a long book can use a good part of it the first
time. If the limit is reached, the reader shows "Gemini free-tier limit reached". Press **Retry**
later: the chapters already summarized are not done again.

## Privacy

What is sent to Google: the text of the current chapter up to your page, your question or the term
you selected, and the book and chapter titles. When chapters are summarized, the text of each
earlier chapter is sent too, once. In practice, **the book up to your page is sent to Google.** On
the free tier, Google may use the text you send to improve its models. Don't use the assistant on
books or documents you don't want to share.

The API key is stored in the reader's settings file on the SD card and is sent in a request header,
never in the URL.

## Limitations

- If you know a famous book, so does Gemini. It is told not to reveal anything after your page and
  to answer only from the text it receives, but that is not a guarantee.
- A book stored as one huge chapter (one file for the whole text) gets no earlier-chapter
  summaries: the assistant sees only the last 120 KB of text (150 pages at most) before your page.
- Pages before the current chapter are read from the original EPUB, so a Lingua translation does
  not change what is summarized.

## For developers

- `src/modules/recap/RecapText.*`: rebuilds plain text from laid-out pages. It rejoins words split
  by inserted hyphens and turns the em-space paragraph indent into a line break. `drain()` lets the
  reader write the current chapter to the SD card page by page.
- `src/modules/recap/HtmlTextExtractor.*`: streaming, chunk-safe XHTML → plain text for chapters
  that are not laid out (skips `<head>`, `<script>`, `<style>` and ruby annotations, turns block
  tags into line breaks, decodes entities, collapses whitespace).
- `src/modules/recap/GeminiRecap.*`: prompts (recap, characters, question, who-is, chapter summary),
  the in-memory recap body and the pieces of a streamed body (`streamingBodyPrefix()`,
  `appendJsonEscaped()`, `streamingBodySuffix()`), response/error parsing, the model lists and the
  Markdown stripping. Covered by `test/chapter_recap`.
- `src/modules/recap/BookAssistantActivity.*`: Wi-Fi, one worker task per request (framebuffer
  released, like the Lingua translation activities) and the paged answer. For the context modes,
  the worker loads the EPUB metadata-only and fills in missing `<book cache>/assistant/ch<N>.sum`
  files: an empty file marks a chapter with no story text. It then assembles
  `<book cache>/assistant/body.json` (summaries plus `current.txt`) on the SD card and sends it with
  `HttpDownloader::postFile()`, which streams it with `esp_http_client_write()` (the simulator
  sends it buffered). Between chapters the worker asks the main task to repaint the progress
  screen, which restores the framebuffer, draws and releases it again.
- `EpubReaderActivity::openBookAssistant()` and the functions after it: the picker, the key
  prompt, the question keyboard, writing `current.txt`, and the hand-off. "Who is" reuses
  `startClipSelection()` with `forAssistantWhoIs`: the selection is not saved as a clipping.
