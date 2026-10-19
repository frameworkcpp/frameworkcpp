# Writing Documentation

FrameworkCpp's documentation is written for humans first. Its job is not to
catalogue the API — it is to make a reader **understand an idea and then use
it**. A page that lists every type signature is a reference, not documentation.

This guide explains how the pages are written, so a contribution reads like the
rest of them. Follow it whether you are adding a module, a section, or a single
paragraph.

## Start from the problem

Open with the reader's situation, not with the name of a type. Name the pain in
plain language first; introduce the framework's answer in one sentence after.

> **Weak:** "`arena_t` is the class that manages memory."
>
> **Better:** "On a real-time system, `malloc` is a gamble: sometimes it returns
> in microseconds, sometimes in milliseconds, and that spike is what misses your
> deadline. `arena_t` reserves memory once and hands it out in slices, so an
> allocation never touches the heap."

The reader should recognise their own problem before they meet your API.

## One idea per section

A section teaches **one** thing. If you find yourself explaining a second
concept, that is a new heading. Short sections are easier to read, easier to
link to, and easier to keep correct.

## Introduce code with a colon

Never drop a code block without telling the reader what it does. Lead in with a
sentence that ends in a colon and states the outcome:

```cpp
arena_t arena;
auto shard = arena.create_shard(1, {{0, sizeof(Command), alignof(Command), 512}});
auto *cmd = shard.allocate<Command>();   // no malloc, no lock, no exception
```

Every example should be the smallest thing that runs. Show the idea, not a
whole program. If the reader has to delete three lines to see the point, cut
them yourself.

## Explain the why before the how

State the reason behind a design choice before you show the mechanism:

> Each bucket holds a single size, so finding a free slot is "the first one that
> fits" instead of a search across mixed sizes.

A reader who understands *why* will use the tool correctly without memorising
rules.

## Escalate

Order a section from the simplest working case to the variants to the edge
cases. Start with the call that just works, then add one option at a time:

1. the default;
2. how to change it;
3. what happens when it goes wrong.

Do not open with the most configurable form. Earn it.

## Anticipate the next question

When a reader is about to wonder "but what about X?", answer it in the next
sentence. When two ideas look alike, say how they differ before they conflate
them. Documentation that only answers the obvious question leaves the reader
stuck one step later.

## Reach for an analogy

When a concept is abstract, anchor it to something concrete the reader already
knows — a warehouse, a checkout queue, a stack of plates. Use the analogy to
introduce the idea, then return to the real terms.

## Say the trade-offs

Tell the reader when **not** to use what you are describing. Naming the limit
builds trust and prevents misuse:

> If your workload allocates and frees unpredictable, wildly different sizes, an
> arena will fragment your budget; a fixed-size pool fits better.

## Voice and tone

- **Second person, present tense.** "You create a shard", not "shards are
  created by the user".
- **Modal, low-pressure.** "you can", "you may", "if you would like to". Avoid
  commands like "you must".
- **No hype.** Describe what the code does, not how great it is.
- **Technical names in `inline code`** — types, functions, config keys, paths.
- **Short paragraphs.** One idea each. If a paragraph has two "and then", split
  it.

## Code examples

- Every fence carries a language: `cpp`, `bash`, `cmake`, `text`.
- Use the framework's real names (`arena_t`, `get_capacity()`), not placeholders.
- Prefer complete, copy-pasteable snippets over fragments.
- Comment the non-obvious line, not the obvious one.

## The shape of a page

A page reads top to bottom: the idea first, then the pieces, then the details.
The first paragraph orients the reader; the last one can point to what comes
next. Headings should read like tasks or questions, not like identifiers.

## Checklist before you publish

- Does it open from a real problem?
- One idea per section?
- Is every code block introduced by a colon sentence and runnable?
- Is the *why* there, not just the *how*?
- Does it escalate from simple to edge cases?
- Are the trade-offs stated?
- Second person, present tense, no hype?
- Does every fence have a language tag?
- Do the links point to pages that exist?

## Previewing the site

The documentation is built with VitePress. To preview your changes locally:

```bash
npm install
npm run docs:dev      # live preview at http://localhost:5173
npm run docs:build    # production build, catches broken links
```

Run `docs:build` before you open a pull request: it fails on links to pages
that do not exist.
