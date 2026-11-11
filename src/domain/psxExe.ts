import { err, ok, type Result } from "./result.js";

/** Sony PS-X EXE header size. The payload begins at this file offset. */
export const PSX_EXE_HEADER_SIZE = 2048;

const MAGIC = Buffer.from("PS-X EXE", "ascii");
const PAYLOAD_ALIGNMENT = 2048;
const ADDRESS_ALIGNMENT = 4;
const ELF32_HEADER_SIZE = 52;
const ELF32_PROGRAM_HEADER_SIZE = 32;
const ELF_MACHINE_MIPS = 8;
const ET_EXEC = 2;
const PT_LOAD = 1;
const PF_R = 4;
const PF_W = 2;
const PF_X = 1;
/** File offset of the single PT_LOAD payload. 52 + 32, which is 4-byte aligned. */
const PAYLOAD_FILE_OFFSET = ELF32_HEADER_SIZE + ELF32_PROGRAM_HEADER_SIZE;
const UINT32_LIMIT = 0x1_0000_0000n;

/** Load facts read from a PS-X EXE header. They do not describe instructions. */
export interface PsxExeFacts {
  readonly initialPc: number;
  readonly initialGp: number;
  readonly loadAddress: number;
  readonly payloadSize: number;
  readonly dataAddress: number;
  readonly dataSize: number;
  readonly bssAddress: number;
  readonly bssSize: number;
  readonly stackPointer: number;
  readonly stackSize: number;
  readonly regionMarker: string | null;
  /** Bytes mapped by PT_LOAD, including a contiguous trailing BSS when present. */
  readonly memorySize: number;
}

/** ELF32 image whose declarations the existing recordless MIPS-I lane can read. */
export interface PsxExeElfImage {
  readonly facts: PsxExeFacts;
  readonly elf: Buffer;
}

const asBuffer = (input: Uint8Array): Buffer =>
  Buffer.isBuffer(input) ? input : Buffer.from(input);

const u32 = (bytes: Buffer, offset: number): number =>
  bytes.readUInt32LE(offset);

const fits = (start: bigint, size: bigint): boolean =>
  size >= 0n && start >= 0n && start <= UINT32_LIMIT - size;

const regionMarker = (bytes: Buffer): Result<string | null, string> => {
  const start = 0x4c;
  let end = start;
  while (end < PSX_EXE_HEADER_SIZE && bytes[end] !== 0) {
    const value = bytes[end] ?? 0;
    if (value < 0x20 || value > 0x7e)
      return err("PS-X region marker is not printable ASCII");
    end += 1;
  }
  return ok(end === start ? null : bytes.toString("ascii", start, end));
};

interface PsxExeWords {
  readonly initialPc: number;
  readonly initialGp: number;
  readonly loadAddress: number;
  readonly payloadSize: number;
  readonly dataAddress: number;
  readonly dataSize: number;
  readonly bssAddress: number;
  readonly bssSize: number;
  readonly stackPointer: number;
  readonly stackSize: number;
}

const memorySizeFor = (
  words: PsxExeWords,
  fileLength: number,
): Result<number, string> => {
  const {
    initialPc,
    loadAddress,
    payloadSize,
    dataAddress,
    dataSize,
    bssAddress,
    bssSize,
  } = words;
  if (payloadSize === 0 || payloadSize % PAYLOAD_ALIGNMENT !== 0)
    return err(
      `PS-X payload size must be a positive multiple of ${PAYLOAD_ALIGNMENT} bytes`,
    );
  if (fileLength !== PSX_EXE_HEADER_SIZE + payloadSize)
    return err(
      "PS-X file length does not match the 2048-byte header plus the declared payload size",
    );
  if (
    loadAddress % ADDRESS_ALIGNMENT !== 0 ||
    initialPc % ADDRESS_ALIGNMENT !== 0
  )
    return err("PS-X load address and initial PC must be 4-byte aligned");
  const load = BigInt(loadAddress);
  const payload = BigInt(payloadSize);
  const entry = BigInt(initialPc);
  if (!fits(load, payload))
    return err("PS-X load range exceeds the 32-bit address space");
  if (entry < load || entry + BigInt(ADDRESS_ALIGNMENT) > load + payload)
    return err("PS-X initial PC is outside the loaded image");
  if (dataSize === 0 && dataAddress !== 0)
    return err("PS-X data address is set without a data size");
  const dataStart = BigInt(dataAddress);
  const dataLength = BigInt(dataSize);
  if (
    dataSize !== 0 &&
    (!fits(dataStart, dataLength) ||
      dataStart < load ||
      dataStart + dataLength > load + payload)
  )
    return err("PS-X data range is outside the loaded image");
  if (bssSize === 0)
    return bssAddress === 0
      ? ok(payloadSize)
      : err("PS-X BSS address is set without a BSS size");
  const bss = BigInt(bssAddress);
  const bssLength = BigInt(bssSize);
  if (bss !== load + payload)
    return err("PS-X BSS is not contiguous with the loaded image");
  if (!fits(bss, bssLength))
    return err("PS-X BSS exceeds the 32-bit address space");
  return ok(payloadSize + bssSize);
};

/**
 * Read a PS-X EXE header. The payload is the following bytes; this does not
 * decode instructions or infer a CPU from the marker text.
 */
export const parsePsxExe = (input: Uint8Array): Result<PsxExeFacts, string> => {
  const bytes = asBuffer(input);
  if (bytes.length < PSX_EXE_HEADER_SIZE)
    return err(
      `PS-X EXE header is truncated: needs ${PSX_EXE_HEADER_SIZE} bytes; the file has ${bytes.length}`,
    );
  if (!bytes.subarray(0, MAGIC.length).equals(MAGIC))
    return err("PS-X EXE signature is absent");
  if (u32(bytes, 0x08) !== 0 || u32(bytes, 0x0c) !== 0)
    return err(
      "PS-X header fields at 0x08 and 0x0c are non-zero; this converter only loads the single payload",
    );
  const words: PsxExeWords = {
    initialPc: u32(bytes, 0x10),
    initialGp: u32(bytes, 0x14),
    loadAddress: u32(bytes, 0x18),
    payloadSize: u32(bytes, 0x1c),
    dataAddress: u32(bytes, 0x20),
    dataSize: u32(bytes, 0x24),
    bssAddress: u32(bytes, 0x28),
    bssSize: u32(bytes, 0x2c),
    stackPointer: u32(bytes, 0x30),
    stackSize: u32(bytes, 0x34),
  };
  const memorySize = memorySizeFor(words, bytes.length);
  if (!memorySize.ok) return memorySize;
  const marker = regionMarker(bytes);
  if (!marker.ok) return marker;
  return ok({
    ...words,
    regionMarker: marker.value,
    memorySize: memorySize.value,
  });
};

/**
 * Wrap a PS-X EXE payload in ELF32 little-endian MIPS `ET_EXEC` form.
 *
 * `e_flags` is zero: MIPS-I with an unspecified ABI and no machine-extension
 * bits. That is the declaration the existing recordless Ghidra lane inspects;
 * it is not proof that every instruction is MIPS-I, and it does not describe
 * R3000 COP2. The section table is empty, which REA treats as an inspected
 * absence of ABI records. A string table was enough for Ghidra 12.1.4 to drop
 * the program-header execute permission and the ELF entry point; program
 * headers alone keep both. The 2048-byte header is not loaded. Initial GP and
 * SP stay in the returned facts because this ELF has no PS-X program header.
 */
export const convertPsxExeToElf32Mips = (
  input: Uint8Array,
): Result<PsxExeElfImage, string> => {
  const parsed = parsePsxExe(input);
  if (!parsed.ok) return parsed;
  const facts = parsed.value;
  const imageSize = PAYLOAD_FILE_OFFSET + facts.payloadSize;
  if (!Number.isSafeInteger(imageSize))
    return err("PS-X payload is too large to place in an ELF32 image");
  const image = Buffer.alloc(imageSize);
  image.set([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1], 0);
  image.writeUInt16LE(ET_EXEC, 16);
  image.writeUInt16LE(ELF_MACHINE_MIPS, 18);
  image.writeUInt32LE(1, 20);
  image.writeUInt32LE(facts.initialPc, 24);
  image.writeUInt32LE(ELF32_HEADER_SIZE, 28);
  image.writeUInt16LE(ELF32_HEADER_SIZE, 40);
  image.writeUInt16LE(ELF32_PROGRAM_HEADER_SIZE, 42);
  image.writeUInt16LE(1, 44);
  const program = ELF32_HEADER_SIZE;
  image.writeUInt32LE(PT_LOAD, program);
  image.writeUInt32LE(PAYLOAD_FILE_OFFSET, program + 4);
  image.writeUInt32LE(facts.loadAddress, program + 8);
  image.writeUInt32LE(facts.loadAddress, program + 12);
  image.writeUInt32LE(facts.payloadSize, program + 16);
  image.writeUInt32LE(facts.memorySize, program + 20);
  image.writeUInt32LE(PF_R | PF_W | PF_X, program + 24);
  image.writeUInt32LE(ADDRESS_ALIGNMENT, program + 28);
  asBuffer(input).copy(
    image,
    PAYLOAD_FILE_OFFSET,
    PSX_EXE_HEADER_SIZE,
    PSX_EXE_HEADER_SIZE + facts.payloadSize,
  );
  return ok({ facts, elf: image });
};
