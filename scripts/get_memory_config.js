#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/**
 * Get memory configuration for build.sh
 *
 * Reads js/memory_layout.js and calculates INITIAL_MEMORY and MAXIMUM_MEMORY
 * for emscripten. This ensures build-time and runtime memory configs stay synchronized.
 *
 * Usage: node scripts/get_memory_config.js [initial|max|arena]
 *   no args  → outputs INITIAL_MEMORY (bytes)
 *   initial  → outputs INITIAL_MEMORY (bytes)
 *   max      → outputs MAXIMUM_MEMORY (bytes)
 *   arena    → outputs the placement arena size (bytes), which the module
 *              claims once at init and hands to clockwork::mem::set_arena. Read
 *              here for the same reason as the rest: the build and the
 *              runtime must not disagree about how the address space is cut.
 */

import { MemoryLayout } from '../js/memory_layout.js';

try {
    const memory = MemoryLayout;

    // Validate memory config exists
    if (!memory || typeof memory !== 'object') {
        console.error('Error: MemoryLayout is missing or invalid');
        process.exit(1);
    }

    // Calculate total memory in bytes
    // Either use totalMemory getter or calculate from totalPages
    let totalMemory;

    if (typeof memory.totalMemory === 'number') {
        totalMemory = memory.totalMemory;
    } else if (typeof memory.totalPages === 'number') {
        totalMemory = memory.totalPages * 65536;
    } else {
        console.error('Error: Unable to determine total memory from config');
        console.error('  memory.totalMemory:', memory.totalMemory);
        console.error('  memory.totalPages:', memory.totalPages);
        process.exit(1);
    }

    // Validate it's a reasonable value
    const MIN_MEMORY = 16 * 1024 * 1024;  // 16MB minimum
    const MAX_MEMORY = 2 * 1024 * 1024 * 1024;  // 2GB maximum

    if (totalMemory < MIN_MEMORY) {
        console.error(`Error: Total memory ${totalMemory} bytes is too small (minimum ${MIN_MEMORY})`);
        process.exit(1);
    }

    if (totalMemory > MAX_MEMORY) {
        console.error(`Error: Total memory ${totalMemory} bytes is too large (maximum ${MAX_MEMORY})`);
        process.exit(1);
    }

    // Must be a multiple of 64KB (WebAssembly page size)
    if (totalMemory % 65536 !== 0) {
        console.error(`Error: Total memory ${totalMemory} bytes is not a multiple of 65536 (WebAssembly page size)`);
        process.exit(1);
    }

    // ========================================================================
    // Internal Consistency Validation
    // ========================================================================

    // Validate totalPages matches totalMemory
    if (typeof memory.totalPages === 'number') {
        const expectedFromPages = memory.totalPages * 65536;
        if (totalMemory !== expectedFromPages) {
            console.error(`Error: Memory layout inconsistency detected`);
            console.error(`  totalPages * 65536 = ${expectedFromPages} bytes`);
            console.error(`  totalMemory = ${totalMemory} bytes`);
            console.error(`  These must match!`);
            process.exit(1);
        }
    }

    // Validate assetsOffset + assetsSize == totalMemory. The ASSETS is the top
    // region, because it is the one that grows — see MemoryLayout.assetsOffset.
    if (typeof memory.assetsOffset === 'number' && typeof memory.assetsSize === 'number') {
        const expectedTotal = memory.assetsOffset + memory.assetsSize;
        if (totalMemory !== expectedTotal) {
            console.error(`Error: Memory layout inconsistency detected`);
            console.error(`  assetsOffset + assetsSize = ${expectedTotal} bytes`);
            console.error(`  totalMemory = ${totalMemory} bytes`);
            console.error(`  These must match!`);
            process.exit(1);
        }
    }

    // Validate ringBufferReserved is present and reasonable
    if (typeof memory.ringBufferReserved === 'number') {
        if (memory.ringBufferReserved < 64 * 1024) {
            console.error(`Error: ringBufferReserved (${memory.ringBufferReserved}) is too small (minimum 64KB)`);
            process.exit(1);
        }
        if (memory.ringBufferReserved > 128 * 1024 * 1024) {
            console.error(`Error: ringBufferReserved (${memory.ringBufferReserved}) is unreasonably large (maximum 128MB)`);
            process.exit(1);
        }
    }

    // Validate guestMemoryOffset is after ring buffer space
    if (typeof memory.guestMemoryOffset === 'number' && typeof memory.ringBufferReserved === 'number') {
        if (memory.guestMemoryOffset <= memory.ringBufferReserved) {
            console.error(`Error: guestMemoryOffset (${memory.guestMemoryOffset}) must be greater than ringBufferReserved (${memory.ringBufferReserved})`);
            process.exit(1);
        }
    }

    // Validate wasmHeapSize (if getter exists) is reasonable
    if (typeof memory.wasmHeapSize === 'number') {
        const wasmHeap = memory.wasmHeapSize;
        if (wasmHeap < 1 * 1024 * 1024) {
            console.error(`Error: wasmHeapSize (${wasmHeap}) is too small (minimum 1MB)`);
            process.exit(1);
        }
        if (wasmHeap > totalMemory) {
            console.error(`Error: wasmHeapSize (${wasmHeap}) exceeds totalMemory (${totalMemory})`);
            process.exit(1);
        }
        // Validate it matches the calculation.
        //
        // Stated as a SUM OF THE PARTS rather than as a subtraction of the two
        // that were interesting at the time: the guest's arena sits on top of
        // everything below it, so a region added below and forgotten here is
        // exactly the mistake this check exists to catch. Subtracting a fixed
        // list silently stops checking the moment the list grows — which is
        // how it read when the assets area and the returns area were added.
        // The assets area is TOP, so what sits below it is everything else. Checking
        // the growable region's offset is the check that matters: if anything
        // below it changes size and this does not follow, every published
        // range moves under a guest that is already holding pointers into it.
        if (typeof memory.assetsOffset === 'number' && typeof memory.ringBufferReserved === 'number') {
            const below = [
                ['wasmHeapSize',       wasmHeap],
                ['ringBufferReserved', memory.ringBufferReserved],
                ['memArenaSize',       memory.memArenaSize ?? 0],
                ['returnsSize',         memory.returnsSize ?? 0],
                ['guestMemorySize',    memory.guestMemorySize ?? 0],
            ];
            const expectedOffset = below.reduce((n, [, v]) => n + v, 0);
            if (memory.assetsOffset !== expectedOffset) {
                console.error(`Error: assetsOffset does not match the regions below it`);
                console.error(`  assetsOffset getter returns:  ${memory.assetsOffset} bytes`);
                console.error(`  Sum of the regions below it:      ${expectedOffset} bytes`);
                for (const [name, v] of below) console.error(`    ${name.padEnd(20)} ${v}`);
                process.exit(1);
            }
        }
    }

    // Validate guestMemorySize is reasonable. 0 is the default: no fixed
    // slice, clockwork reserves the guest's working memory itself, from the
    // placement arena, sized to the guest's want (js/memory_layout.js).
    if (typeof memory.guestMemorySize === 'number' && memory.guestMemorySize !== 0) {
        if (memory.guestMemorySize < 1 * 1024 * 1024) {
            console.error(`Error: guestMemorySize (${memory.guestMemorySize}) is too small (minimum 1MB)`);
            process.exit(1);
        }
        if (memory.guestMemorySize > totalMemory) {
            console.error(`Error: guestMemorySize (${memory.guestMemorySize}) exceeds totalMemory (${totalMemory})`);
            process.exit(1);
        }
    }

    // Validate maxAssetsSize if present. The assets area is the region that grows,
    // so it is the one carrying a ceiling — see MemoryLayout.assetsOffset.
    if (typeof memory.maxAssetsSize === 'number') {
        if (memory.maxAssetsSize < memory.assetsSize) {
            console.error(`Error: maxAssetsSize (${memory.maxAssetsSize}) must be >= assetsSize (${memory.assetsSize})`);
            process.exit(1);
        }
        const maxTotalMemory = memory.maxTotalMemory || (memory.assetsOffset + memory.maxAssetsSize);
        if (maxTotalMemory % 65536 !== 0) {
            console.error(`Error: maxTotalMemory (${maxTotalMemory}) must be a multiple of 65536 (WebAssembly page size)`);
            process.exit(1);
        }
        if (maxTotalMemory > MAX_MEMORY) {
            console.error(`Error: maxTotalMemory (${maxTotalMemory}) exceeds maximum (${MAX_MEMORY})`);
            process.exit(1);
        }
    }

    // The arena is claimed with one malloc out of the gap below the guest
    // region, so the gap has to be big enough to hold it and everything else.
    if (typeof memory.memArenaSize === 'number') {
        if (memory.memArenaSize < 0) {
            console.error(`Error: memArenaSize (${memory.memArenaSize}) cannot be negative`);
            process.exit(1);
        }
        if (memory.memArenaSize >= memory.guestMemoryOffset) {
            console.error(`Error: memArenaSize (${memory.memArenaSize}) leaves no room below guestMemoryOffset (${memory.guestMemoryOffset})`);
            process.exit(1);
        }
    }

    // Output based on argument
    const arg = process.argv[2];
    if (arg === 'arena') {
        console.log(memory.memArenaSize ?? 0);
    } else if (arg === 'max') {
        // 1GB ceiling — virtual address space, not committed memory.
        // Accommodates large RT pools (up to 256MB) and large sample libraries.
        const maxTotalMemory = 2 * 1024 * 1024 * 1024; // 2GB
        console.log(maxTotalMemory);
    } else {
        // Default: output initial memory
        console.log(totalMemory);
    }

} catch (error) {
    console.error('Error reading memory configuration:', error.message);
    process.exit(1);
}
