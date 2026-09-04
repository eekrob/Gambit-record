import { describe, expect, it } from "vitest";
import { canConsume, classifyChunk, fixedTimeEqual, isExpired, limits, nextOffsetFromRange, parseContentRange } from "../src/protocol";

describe("broker protocol", () => {
  it("rejects malformed and oversized ranges", () => {
    expect(parseContentRange("bytes 0-8388607/9000000")).toEqual({ start: 0, end: 8388607, total: 9000000 });
    expect(parseContentRange("bytes 4-3/10")).toBeNull();
    expect(parseContentRange("bytes 0-10/10")).toBeNull();
  });
  it("authenticates exact keys", () => {
    expect(fixedTimeEqual("release-key", "release-key")).toBe(true);
    expect(fixedTimeEqual("release-key-x", "release-key")).toBe(false);
    expect(fixedTimeEqual("", "release-key")).toBe(false);
  });
  it("uses configured limits", () => {
    expect(limits({ MAX_CHUNK_BYTES: "1024", MAX_VIDEO_BYTES: "4096", MAX_UPLOADS_PER_IP_DAY: "2", MAX_UPLOADS_GLOBAL_DAY: "3" }))
      .toEqual({ chunk: 1024, video: 4096, perIp: 2, global: 3 });
  });
  it("tracks 308 resumable offsets", () => {
    expect(nextOffsetFromRange("bytes=0-8388607", 0)).toBe(8388608);
    expect(nextOffsetFromRange(null, 25)).toBe(25);
  });
  it("accepts sequential chunks and handles a repeated chunk", () => {
    const first = parseContentRange("bytes 0-7/16")!;
    expect(classifyChunk(first, 0, 16, 8, 8)).toBe("accept");
    expect(classifyChunk(first, 8, 16, 8, 8)).toBe("replay");
    expect(classifyChunk(parseContentRange("bytes 12-15/16"), 8, 16, 4, 8)).toBe("gap");
  });
  it("models expired sessions and rate limits", () => {
    expect(isExpired(99, 100)).toBe(true);
    expect(isExpired(101, 100)).toBe(false);
    expect(canConsume(9, 89, 10, 90)).toBe(true);
    expect(canConsume(10, 1, 10, 90)).toBe(false);
    expect(canConsume(1, 90, 10, 90)).toBe(false);
  });
  it("keeps the same offset after a network interruption", () => {
    const expectedBeforeRequest = 8;
    expect(nextOffsetFromRange(null, expectedBeforeRequest)).toBe(expectedBeforeRequest);
  });
});
