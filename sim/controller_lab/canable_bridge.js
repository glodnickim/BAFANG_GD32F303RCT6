#!/usr/bin/env node
/*
 * EVistDrive Controller Lab - Canable settings bridge.
 *
 * Turns the settings the rider actually has in Canable into the two wire blobs the
 * controller consumes, by calling CANABLE-WEB'S OWN SERIALIZERS. This file deliberately
 * contains no field list, no offset table, no scaling and no CRC: if it did, the wire
 * format would have a third definition (firmware, canable-web, and here) and the three
 * would drift. Canable-web owns the host-side encoding; the firmware owns the decoding.
 *
 * Input  (stdin JSON or --preset <file>): an eVistDrive preset, i.e. { banks: [...], tuning: {...} }
 *         - the exact shape ui/js/evistdrive/presets.js exports and imports.
 * Output (stdout JSON): { tuning_blob, bank_blob, meta }  with blobs as lowercase hex.
 *
 * Canable-web location: --canable-root, else EVIST_CANABLE_ROOT, else the sibling
 * "canable-web" directory of this repository's workspace.
 */
'use strict';

const fs = require('fs');
const path = require('path');

function fail(msg) {
    process.stdout.write(JSON.stringify({ error: msg }) + '\n');
    process.exit(1);
}

function parseArgs(argv) {
    const out = { preset: null, canableRoot: null, bank: 0 };
    for (let i = 2; i < argv.length; i++) {
        const a = argv[i];
        if (a === '--preset') out.preset = argv[++i];
        else if (a === '--canable-root') out.canableRoot = argv[++i];
        else if (a === '--bank') out.bank = parseInt(argv[++i], 10) || 0;
    }
    return out;
}

function resolveCanableRoot(explicit) {
    const candidates = [];
    if (explicit) candidates.push(explicit);
    if (process.env.EVIST_CANABLE_ROOT) candidates.push(process.env.EVIST_CANABLE_ROOT);
    // sim/controller_lab -> motor-controller-firmware -> workspace -> canable-web
    candidates.push(path.resolve(__dirname, '..', '..', '..', 'canable-web'));
    for (const c of candidates) {
        if (c && fs.existsSync(path.join(c, 'canbus.js'))) return c;
    }
    return null;
}

function readInput(presetPath) {
    if (presetPath) {
        if (!fs.existsSync(presetPath)) fail(`preset not found: ${presetPath}`);
        return fs.readFileSync(presetPath, 'utf8');
    }
    try {
        return fs.readFileSync(0, 'utf8');
    } catch (e) {
        fail(`cannot read preset from stdin: ${e.message}`);
    }
    return '';
}

function toHex(bytes) {
    return Array.from(bytes, (b) => (b & 0xFF).toString(16).padStart(2, '0')).join('');
}

function main() {
    const args = parseArgs(process.argv);

    const root = resolveCanableRoot(args.canableRoot);
    if (!root) {
        fail('canable-web not found. Pass --canable-root <path> or set EVIST_CANABLE_ROOT.');
    }

    let CanBusService;
    try {
        // canbus.js exports a singleton; the serializers are static members of its class.
        // Constructing the service does not open USB (GSUsb only opens in init()).
        const instance = require(path.join(root, 'canbus.js'));
        CanBusService = instance.constructor;
    } catch (e) {
        fail(`cannot load canable-web serializers from ${root}: ${e.message}`);
    }

    if (typeof CanBusService.serializeTuningBlob !== 'function'
        || typeof CanBusService.serializeBankBlob !== 'function') {
        fail('canable-web is present but does not expose serializeTuningBlob/serializeBankBlob');
    }

    let preset;
    try {
        preset = JSON.parse(readInput(args.preset));
    } catch (e) {
        fail(`preset is not valid JSON: ${e.message}`);
    }

    const result = { tuning_blob: '', bank_blob: '', meta: {} };

    if (preset.tuning) {
        try {
            result.tuning_blob = toHex(CanBusService.serializeTuningBlob(preset.tuning));
        } catch (e) {
            fail(`serializeTuningBlob failed: ${e.message}`);
        }
    }

    const banks = Array.isArray(preset.banks) ? preset.banks : [];
    const bank = banks[args.bank] || banks.find((b) => b) || null;
    if (bank) {
        try {
            result.bank_blob = toHex(CanBusService.serializeBankBlob(bank));
        } catch (e) {
            fail(`serializeBankBlob failed: ${e.message}`);
        }
    }

    if (!result.tuning_blob && !result.bank_blob) {
        fail('preset carries neither "tuning" nor "banks" - nothing to apply');
    }

    result.meta = {
        canable_root: root,
        bank_index: args.bank,
        preset_name: preset.name || null,
        preset_created: preset.created || null,
        controller_sw_version: preset.source ? preset.source.controller_sw_version || null : null,
        tuning_bytes: result.tuning_blob.length / 2,
        bank_bytes: result.bank_blob.length / 2,
    };

    process.stdout.write(JSON.stringify(result) + '\n');
}

main();
