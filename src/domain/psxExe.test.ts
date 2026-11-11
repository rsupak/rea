import { describe, expect, it } from "vitest";

import { parseExecutableHeader } from "./binaryTarget.js";
import { validateElfDeclaredRanges } from "./elfDeclaredRanges.js";
import { readElfFixedHeader } from "./elfFixedHeader.js";
import { readElfProgramHeader } from "./elfProgramHeader.js";
import { admitRawBinary } from "./rawBinary.js";
import {
  convertPsxExeToElf32Mips,
  parsePsxExe,
  PSX_EXE_HEADER_SIZE,
} from "./psxExe.js";

/** Hand-authored MIPS-I words: addiu sp,sp,-16; jr ra; addiu sp,sp,16. */
const LEAF = Buffer.from([
  0xf0, 0xff, 0xbd, 0x27, 0x08, 0x00, 0xe0, 0x03, 0x10, 0x00, 0xbd, 0x27,
]);

const payload = (words: Buffer, entryOffset = 0): Buffer => {
  const bytes = Buffer.alloc(2048);
  words.copy(bytes, entryOffset);
  return bytes;
};

const exe = (
  body: Buffer,
  fields: {
    readonly pc?: number;
    readonly gp?: number;
    readonly load?: number;
    readonly size?: number;
    readonly dataAddress?: number;
    readonly dataSize?: number;
    readonly bssAddress?: number;
    readonly bssSize?: number;
    readonly sp?: number;
    readonly stackSize?: number;
    readonly marker?: string;
    readonly prefix?: number;
    readonly extra?: number;
  } = {},
): Buffer => {
  const file = Buffer.alloc(
    PSX_EXE_HEADER_SIZE + body.length + (fields.extra ?? 0),
  );
  file.write("PS-X EXE", 0, "ascii");
  file.writeUInt32LE(fields.prefix ?? 0, 0x08);
  file.writeUInt32LE(fields.prefix ?? 0, 0x0c);
  file.writeUInt32LE(fields.pc ?? 0x8001_0000, 0x10);
  file.writeUInt32LE(fields.gp ?? 0, 0x14);
  file.writeUInt32LE(fields.load ?? 0x8001_0000, 0x18);
  file.writeUInt32LE(fields.size ?? body.length, 0x1c);
  file.writeUInt32LE(fields.dataAddress ?? 0, 0x20);
  file.writeUInt32LE(fields.dataSize ?? 0, 0x24);
  file.writeUInt32LE(fields.bssAddress ?? 0, 0x28);
  file.writeUInt32LE(fields.bssSize ?? 0, 0x2c);
  file.writeUInt32LE(fields.sp ?? 0x801f_ff00, 0x30);
  file.writeUInt32LE(fields.stackSize ?? 0, 0x34);
  if (fields.marker !== undefined) file.write(fields.marker, 0x4c, "ascii");
  body.copy(file, PSX_EXE_HEADER_SIZE);
  return file;
};

const admittedRanges = (elf: Buffer) => {
  const steps = validateElfDeclaredRanges(elf, elf.length);
  let next = steps.next();
  while (!next.done) {
    const { offset, size } = next.value;
    next = steps.next(elf.subarray(offset, offset + size));
  }
  return next.value;
};

describe("PS-X EXE parsing", () => {
  it("reads the load, entry, stack, and marker without treating them as code", () => {
    const parsed = parsePsxExe(
      exe(payload(LEAF), {
        pc: 0x8001_0010,
        gp: 0x8001_1000,
        sp: 0x801f_fff0,
        stackSize: 0x1000,
        marker: "synthetic",
      }),
    );
    expect(parsed.ok).toBe(true);
    if (!parsed.ok) return;
    expect(parsed.value).toMatchObject({
      initialPc: 0x8001_0010,
      initialGp: 0x8001_1000,
      loadAddress: 0x8001_0000,
      payloadSize: 2048,
      stackPointer: 0x801f_fff0,
      stackSize: 0x1000,
      regionMarker: "synthetic",
      memorySize: 2048,
    });
  });

  it.each([
    ["truncated header", Buffer.from("PS-X EXE"), "truncated"],
    ["missing signature", exe(payload(LEAF)).fill(0, 0, 8), "signature"],
    ["non-zero prefix", exe(payload(LEAF), { prefix: 1 }), "0x08 and 0x0c"],
    ["short payload", exe(Buffer.alloc(1024)), "multiple of 2048"],
    ["length mismatch", exe(payload(LEAF), { extra: 2048 }), "file length"],
    ["unaligned entry", exe(payload(LEAF), { pc: 0x8001_0002 }), "4-byte"],
    [
      "entry outside image",
      exe(payload(LEAF), { pc: 0x8001_0800 }),
      "outside the loaded image",
    ],
    [
      "data outside image",
      exe(payload(LEAF), { dataAddress: 0x8002_0000, dataSize: 4 }),
      "data range",
    ],
    [
      "gapped BSS",
      exe(payload(LEAF), { bssAddress: 0x8001_1000, bssSize: 16 }),
      "not contiguous",
    ],
    [
      "non-ascii marker",
      Buffer.from(exe(payload(LEAF))).fill(0x01, 0x4c, 0x4d),
      "region marker",
    ],
  ] as const)("rejects %s", (_name, bytes, reason) => {
    expect(parsePsxExe(bytes)).toMatchObject({
      ok: false,
      error: expect.stringContaining(reason),
    });
  });

  it("keeps a contiguous BSS uninitialized beyond the payload", () => {
    const parsed = parsePsxExe(
      exe(payload(LEAF), { bssAddress: 0x8001_0800, bssSize: 32 }),
    );
    expect(parsed).toMatchObject({
      ok: true,
      value: { memorySize: 2080, bssSize: 32 },
    });
  });
});

describe("PS-X EXE to ELF32 MIPS", () => {
  it("maps the payload at the load address and the entry inside it", () => {
    const converted = convertPsxExeToElf32Mips(
      exe(payload(LEAF, 16), { pc: 0x8001_0010, marker: "synthetic" }),
    );
    expect(converted.ok).toBe(true);
    if (!converted.ok) return;
    const { elf, facts } = converted.value;
    expect(elf.subarray(0, 8).equals(Buffer.from("PS-X EXE"))).toBe(false);
    expect(elf.includes(Buffer.from("synthetic"))).toBe(false);
    const header = readElfFixedHeader(elf, elf.length);
    expect(header).toMatchObject({
      ok: true,
      value: {
        elfClass: 32,
        byteOrder: "little",
        machine: 8,
        architecture: "mips",
        type: 2,
        flags: 0,
      },
    });
    expect(elf.readUInt32LE(24)).toBe(facts.initialPc);
    const program = readElfProgramHeader(elf.subarray(52, 84), 32, true);
    expect(program).toMatchObject({
      ok: true,
      value: {
        type: 1,
        offset: 84n,
        virtualAddress: BigInt(facts.loadAddress),
        fileSize: BigInt(facts.payloadSize),
        memorySize: BigInt(facts.memorySize),
        flags: 7,
        alignment: 4n,
      },
    });
    expect(elf.readUInt32LE(84 + 16)).toBe(0x27bd_fff0);
    expect(admittedRanges(elf)).toEqual({ ok: true, value: null });
    const direct = parseExecutableHeader(exe(payload(LEAF)), "x64");
    expect(direct.ok).toBe(false);
    if (direct.ok) return;
    expect(direct.error).toContain("unsupported binary format");
  });

  it("still refuses a caller-selected raw-binary MIPS load", () => {
    expect(
      admitRawBinary({
        architecture: "mips",
        entryPoint: "0x80010000",
        fileBytes: 2048,
      }),
    ).toMatchObject({
      ok: false,
      error: {
        constraint: "load_selection_rejected",
        reason: expect.stringContaining("m68k"),
      },
    });
  });

  it("leaves the section table empty so program-header permissions stand", () => {
    const converted = convertPsxExeToElf32Mips(exe(payload(LEAF)));
    expect(converted.ok).toBe(true);
    if (!converted.ok) return;
    const { elf } = converted.value;
    expect(elf.readUInt32LE(32)).toBe(0);
    expect(elf.readUInt16LE(48)).toBe(0);
    expect(elf.readUInt16LE(50)).toBe(0);
    expect(elf.length).toBe(84 + 2048);
    expect(admittedRanges(elf)).toEqual({ ok: true, value: null });
  });
});
