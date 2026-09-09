// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_product.h — the name this binary calls itself.
 *
 * Clockwork is a substrate. The thing a user runs is a PRODUCT built on it —
 * a synth, a plugin, or something else — and that product's name is the
 * one that belongs in a banner, in a log line, in the port list another MIDI
 * application shows, and in the name of a shared-memory segment two processes
 * rendezvous on.
 *
 * Until an embedder existed, every one of those was the literal "clockwork",
 * which is correct only for clockwork running standalone. The first embedder's
 * boot introduced itself as clockwork; that is the bug this header fixes.
 *
 * Set CLOCKWORK_PRODUCT_NAME from CMake:
 *
 *     set(CLOCKWORK_PRODUCT_NAME "MyProduct" CACHE STRING "" FORCE)
 *
 * It defaults to "clockwork" so a standalone build is unchanged.
 *
 * The banner the engine prints as it boots is the product's too, and it is
 * three lines of block art rather than a word, which is nothing to put on a
 * compiler command line. A product supplies it in a header of its own and
 * names that header as CLOCKWORK_PRODUCT_HEADER (a quoted path; from CMake,
 * set CLOCKWORK_PRODUCT_HEADER to the file). The header defines
 * CLOCKWORK_PRODUCT_BANNER; anything it leaves undefined keeps clockwork's
 * default, so a product may overlay only what it has an opinion on.
 *
 * NOT the same as --app-name. That is a RUNTIME override for the names
 * published to the OS audio and MIDI registries, so two copies of one product
 * can be told apart. This is what the product is called when nobody says
 * otherwise.
 */
#ifndef CLOCKWORK_PRODUCT_H
#define CLOCKWORK_PRODUCT_H

#ifdef CLOCKWORK_PRODUCT_HEADER
#include CLOCKWORK_PRODUCT_HEADER
#endif

#ifndef CLOCKWORK_PRODUCT_NAME
#define CLOCKWORK_PRODUCT_NAME "clockwork"
#endif

/* Three lines, no trailing newline; each line starts with its own margin. */
#ifndef CLOCKWORK_PRODUCT_BANNER
#define CLOCKWORK_PRODUCT_BANNER \
    "░█▀▀░█░░░█▀█░█▀▀░█░█░█░█░█▀█░█▀▄░█░█\n" \
    "░█░░░█░░░█░█░█░░░█▀▄░█▄█░█░█░█▀▄░█▀▄\n" \
    "░▀▀▀░▀▀▀░▀▀▀░▀▀▀░▀░▀░▀░▀░▀▀▀░▀░▀░▀░▀"
#endif

/* The prefix every diagnostic line carries. Written as a separate macro so the
   brackets and the trailing space are spelled once rather than at every call
   site. */
#define CLOCKWORK_LOG_PREFIX "[" CLOCKWORK_PRODUCT_NAME "] "

#endif /* CLOCKWORK_PRODUCT_H */
