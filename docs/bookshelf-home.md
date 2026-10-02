---
title: Bookshelf Home
nav_order: 1.6
---

# Bookshelf Home

**Bookshelf** is a Home screen style that shows your library as a bookcase.
Turn it on in **Settings → Display → UI Theme → Bookshelf**. Every other
screen keeps the Minimal look.

![Bookshelf Home](screenshots/bookshelf-home.png)

## What is on it

- **A celestial wallpaper** behind everything - suns, moons and their phases,
  constellations, a ringed planet, a little zodiac wheel - in a pale pattern
  that keeps titles readable.
- **At the top, the book you are reading**, kept compact so the shelves get
  most of the screen: its cover, with the title and author beside it and
  CrossInk's reading stats for it (as on the Dashboard home) in a grid below
  them - reading time, time left, progress, pages per minute, daily average,
  start date, estimated finish date and sessions (on readers without a clock,
  sessions and average session instead of the date-based ones).
- **Top shelf, books you have read:** the books in your recent list. A ribbon
  marks a book in progress, a tick a book you marked as finished. The most
  recent one stands face-on with its cover.
- **Bottom shelf, books waiting to be read:** a random handful of books on
  your SD card that you have never opened. One of them stands face-on (its
  cover is prepared the first time and then kept).
- **Pokémon on the shelves** (Pokémon firmware): the first Pokémon in your
  party sits at the end of the top shelf, the second at the start of the
  bottom one, each cut out of its Pokédex card at its original size.

Books stand spine-out with the title (and the author, when the spine is wide
enough) written along the spine, each with its own width, height and binding
- black, gray with a label, or light with gilt bands - and the page edges
showing at the top; face-on books show their page block along the side.
The pick of unread books stays the same until the reader restarts, then a new
one is drawn.

## Using it

**Touch (X4 Pro):** tap a book to open it, tap the book at the top to continue
reading it, and use **Menu**, **Browse** and **Settings** at the bottom. Swipes
work as on the Minimal home (up: menu, right: browse, down: settings).

**Buttons (X3, X4):** the front buttons work as on the Minimal home (Menu,
Browse, Settings, Read). The side buttons move a frame from book to book on the
shelves; **Read** then opens the framed book (with nothing framed it continues
the book at the top).

## Notes

- Books are found by scanning the SD card the first time Home opens after a
  restart (folders up to three levels deep; hidden folders and the firmware's
  own `pokemon`, `sleep` and `fonts` folders are skipped). On a very large
  library only part of the card is scanned, so the pick comes from that part.
- Titles of unread books come from their file names (except the face-on one,
  which shows its real title). Read books show the title and author from the
  book itself.
