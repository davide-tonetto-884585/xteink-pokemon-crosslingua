# Chapter Recap

**Recap** (reader menu) asks Google Gemini to summarize the pages you read most recently in the
current chapter. Use it to pick a book back up after a break.

## Using it

1. Open the reader menu and choose **Recap**. On touch devices it is in the **More** tab of the
   reader drawer.
2. Choose how much to summarize: the last 3, 5, 10 or 20 pages, or **Chapter so far**. The excerpt
   always ends at the page you are on, and it never goes back past the start of the current chapter.
   The reader remembers your choice.
3. The first time, enter your Gemini API key (see below).
4. The reader connects to Wi-Fi, sends the excerpt and shows the recap. Page through it with the
   page/arrow buttons (or swipe/tap on touch devices). **Back** returns to the book on the same page.

The recap is written in the reader's interface language (Settings → Language), whatever language
the book is in. The model is told to use only the excerpt and not to reveal anything that happens
later in the book.

If you are on a page near the start of a chapter, the recap covers fewer pages than you chose,
because it stops at the chapter's first page. At most about 20 KB of text is sent (roughly 10-15
pages). Longer excerpts are cut from the front, so the most recent pages are always included.

## Getting a Gemini API key

1. Go to [Google AI Studio](https://aistudio.google.com/apikey) and sign in with a Google account.
2. Create an API key. The free tier needs no credit card.
3. In the reader, choose **Recap → Gemini API key** and type the key. You can change it there at
   any time.

If you already use **Gemini** as the Lingua translation engine, Recap uses that key until you set a
separate one.

## Models and limits

The reader tries `gemini-3-flash-preview` first. If that model is not available for your key, is
rate-limited or is overloaded, it falls back to `gemini-3.1-flash-lite`, then `gemini-flash-latest`.
Each model has its own free-tier quota, so the fallback keeps Recap working after you hit Flash's
per-minute limit.

Google does not publish fixed free-tier limits. AI Studio shows the current limits for your project,
and they can change at any time. If every model is over its limit, the reader shows "Gemini
free-tier limit reached": wait a minute, then press **Retry**.

## Privacy

The excerpt (the text of the pages you chose), plus the book and chapter titles, is sent to Google.
**On the free tier, Google may use the text you send to improve its models.** Don't use Recap on
documents you don't want to share. The API key is stored in the reader's settings file on the SD
card and is sent in a request header, never in the URL.

## For developers

- `src/modules/recap/RecapText.*`: rebuilds plain text from laid-out pages. It rejoins words split
  by inserted hyphens and turns the layout's em-space paragraph indent into a line break.
- `src/modules/recap/GeminiRecap.*`: builds the `generateContent` request body (system instruction
  plus excerpt, `thinkingLevel: low`) and parses the response. It also holds the model fallback list
  and the Markdown stripping. Covered by `test/chapter_recap`.
- `src/modules/recap/ChapterRecapActivity.*`: Wi-Fi, the request on a worker task (with the
  framebuffer released, like the Lingua translation activities) and the paged result screen.
- `EpubReaderActivity::launchRecap()` extracts the excerpt while the section is loaded. Then it
  tears down the reader and replaces it with the recap activity, the same hand-off Lingua
  translation uses. Back relaunches the reader from disk.
