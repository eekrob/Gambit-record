export const MiB = 1024 * 1024;
export const GiB = 1024 * MiB;

export type ByteRange = { start: number; end: number; total: number };

export function parseContentRange(value: string | null): ByteRange | null {
  if (!value) return null;
  const match = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(value);
  if (!match) return null;
  const [start, end, total] = match.slice(1).map(Number);
  if (![start, end, total].every(Number.isSafeInteger) || start < 0 || end < start || end >= total) return null;
  return { start, end, total };
}

export function fixedTimeEqual(supplied: string, expected: string): boolean {
  const encoder = new TextEncoder();
  const a = encoder.encode(supplied), b = encoder.encode(expected);
  let difference = a.length ^ b.length;
  const length = Math.max(a.length, b.length);
  for (let i = 0; i < length; i++) difference |= (a[i] ?? 0) ^ (b[i] ?? 0);
  return difference === 0;
}

export function limits(env: { MAX_CHUNK_BYTES?: string; MAX_VIDEO_BYTES?: string; MAX_UPLOADS_PER_IP_DAY?: string; MAX_UPLOADS_GLOBAL_DAY?: string }) {
  return {
    chunk: Number(env.MAX_CHUNK_BYTES ?? 8 * MiB),
    video: Number(env.MAX_VIDEO_BYTES ?? 4 * GiB),
    perIp: Number(env.MAX_UPLOADS_PER_IP_DAY ?? 10),
    global: Number(env.MAX_UPLOADS_GLOBAL_DAY ?? 90),
  };
}

export function nextOffsetFromRange(value: string | null, fallback: number): number {
  const match = /^bytes=0-(\d+)$/.exec(value ?? "");
  return match ? Number(match[1]) + 1 : fallback;
}

export type ChunkDecision = "accept" | "replay" | "gap" | "invalid";
export function classifyChunk(range: ByteRange | null, expectedOffset: number, expectedTotal: number,
                              contentLength: number, maxChunk: number): ChunkDecision {
  if (!range || range.total !== expectedTotal || contentLength !== range.end - range.start + 1 || contentLength > maxChunk) return "invalid";
  if (range.start < expectedOffset) return "replay";
  if (range.start > expectedOffset) return "gap";
  return "accept";
}

export function isExpired(expiresAt: number, now = Date.now()): boolean { return expiresAt <= now; }
export function canConsume(ipCount: number, globalCount: number, perIpLimit: number, globalLimit: number): boolean {
  return ipCount < perIpLimit && globalCount < globalLimit;
}
