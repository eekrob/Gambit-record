import { canConsume, classifyChunk, fixedTimeEqual, isExpired, limits, nextOffsetFromRange, parseContentRange } from "./protocol";
import { DurableObject } from "cloudflare:workers";

type Env = {
  UPLOADS: DurableObjectNamespace<UploadSession>;
  LIMITER: DurableObjectNamespace<DailyLimiter>;
  BROKER_KEY: string;
  YOUTUBE_CLIENT_ID: string;
  YOUTUBE_CLIENT_SECRET: string;
  YOUTUBE_REFRESH_TOKEN: string;
  CHANNEL_TITLE: string;
  MAX_CHUNK_BYTES?: string;
  MAX_VIDEO_BYTES?: string;
  MAX_UPLOADS_PER_IP_DAY?: string;
  MAX_UPLOADS_GLOBAL_DAY?: string;
};

type UploadState = {
  uploadUrl: string;
  total: number;
  nextOffset: number;
  expiresAt: number;
  videoId?: string;
  url?: string;
};

let cachedToken: { value: string; expiresAt: number } | undefined;
const jsonResponse = (body: unknown, status = 200, headers?: HeadersInit) =>
  Response.json(body, { status, headers: { "cache-control": "no-store", ...headers } });

async function accessToken(env: Env): Promise<string> {
  if (cachedToken && cachedToken.expiresAt > Date.now() + 60_000) return cachedToken.value;
  const form = new URLSearchParams({ client_id: env.YOUTUBE_CLIENT_ID, client_secret: env.YOUTUBE_CLIENT_SECRET,
    refresh_token: env.YOUTUBE_REFRESH_TOKEN, grant_type: "refresh_token" });
  const response = await fetch("https://oauth2.googleapis.com/token", { method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" }, body: form });
  if (!response.ok) throw new Error(`oauth_${response.status}`);
  const body = await response.json<{ access_token: string; expires_in: number }>();
  cachedToken = { value: body.access_token, expiresAt: Date.now() + body.expires_in * 1000 };
  return cachedToken.value;
}

function authenticated(request: Request, env: Env): boolean {
  return fixedTimeEqual(request.headers.get("x-grecord-key") ?? "", env.BROKER_KEY ?? "");
}

async function createUpload(request: Request, env: Env): Promise<Response> {
  const body = await request.json<Record<string, unknown>>();
  const size = Number(body.size); const max = limits(env).video;
  if (!Number.isSafeInteger(size) || size <= 0 || size > max) return jsonResponse({ error: "invalid_size" }, 413);
  if (body.privacyStatus !== "unlisted") return jsonResponse({ error: "privacy_must_be_unlisted" }, 400);
  const ip = request.headers.get("cf-connecting-ip") ?? "unknown";
  const limiter = env.LIMITER.get(env.LIMITER.idFromName("global"));
  const allowed = await limiter.fetch("https://limiter/consume", { method: "POST", headers: {
    "x-ip": ip, "x-per-ip": String(limits(env).perIp), "x-global": String(limits(env).global) } });
  if (!allowed.ok) return jsonResponse({ error: "daily_upload_limit" }, 429);

  const token = await accessToken(env);
  const metadata = { snippet: { title: String(body.title ?? "Gambit Record").slice(0, 100),
    description: String(body.description ?? "").slice(0, 5000), categoryId: "20" },
    status: { privacyStatus: "unlisted", selfDeclaredMadeForKids: false } };
  const youtube = await fetch("https://www.googleapis.com/upload/youtube/v3/videos?part=snippet,status&uploadType=resumable", {
    method: "POST", headers: { authorization: `Bearer ${token}`, "content-type": "application/json; charset=UTF-8",
      "x-upload-content-type": "video/mp4", "x-upload-content-length": String(size) }, body: JSON.stringify(metadata) });
  const uploadUrl = youtube.headers.get("location");
  if (!youtube.ok || !uploadUrl) return jsonResponse({ error: "youtube_session_failed", upstreamStatus: youtube.status }, 502);
  const id = env.UPLOADS.newUniqueId();
  const object = env.UPLOADS.get(id);
  await object.fetch("https://session/init", { method: "POST", body: JSON.stringify({ uploadUrl, total: size }) });
  return jsonResponse({ id: id.toString(), chunkSize: limits(env).chunk, expiresIn: 86400 }, 201);
}

async function routeUpload(request: Request, env: Env, idText: string, operation?: string): Promise<Response> {
  let id: DurableObjectId; try { id = env.UPLOADS.idFromString(idText); } catch { return jsonResponse({ error: "invalid_upload_id" }, 404); }
  const object = env.UPLOADS.get(id);
  const headers = new Headers(request.headers);
  headers.set("x-access-token", await accessToken(env));
  headers.set("x-max-chunk", String(limits(env).chunk));
  const path = operation === "cancel" ? "/cancel" : request.method === "GET" ? "/status" : "/chunk";
  return object.fetch(`https://session${path}`, { method: request.method, headers, body: request.method === "PUT" ? request.body : undefined });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      if (!authenticated(request, env)) return jsonResponse({ error: "unauthorized" }, 401);
      const url = new URL(request.url);
      if (request.method === "GET" && url.pathname === "/v1/channel") {
        const response = await fetch("https://www.googleapis.com/youtube/v3/channels?part=snippet&mine=true", {
          headers: { authorization: `Bearer ${await accessToken(env)}` } });
        if (!response.ok) return jsonResponse({ error: "youtube_channel_failed" }, 502);
        const body = await response.json<{ items?: Array<{ id: string; snippet: { title: string } }> }>();
        const channel = body.items?.[0]; return channel ? jsonResponse({ id: channel.id, title: channel.snippet.title }) : jsonResponse({ error: "channel_not_found" }, 404);
      }
      if (request.method === "POST" && url.pathname === "/v1/uploads") return createUpload(request, env);
      const match = /^\/v1\/uploads\/([0-9a-f]{64})(?:\/(cancel))?$/.exec(url.pathname);
      if (match && ((request.method === "PUT" && !match[2]) || request.method === "GET" || (request.method === "POST" && match[2])))
        return routeUpload(request, env, match[1], match[2]);
      return jsonResponse({ error: "not_found" }, 404);
    } catch (error) { return jsonResponse({ error: "broker_unavailable", detail: String(error) }, 503); }
  }
} satisfies ExportedHandler<Env>;

export class UploadSession extends DurableObject<Env> {
  async fetch(request: Request): Promise<Response> {
    const path = new URL(request.url).pathname;
    if (path === "/init" && request.method === "POST") {
      const input = await request.json<{ uploadUrl: string; total: number }>();
      const state: UploadState = { ...input, nextOffset: 0, expiresAt: Date.now() + 86_400_000 };
      await this.ctx.storage.put("state", state); await this.ctx.storage.setAlarm(state.expiresAt);
      return jsonResponse({ ok: true });
    }
    const state = await this.ctx.storage.get<UploadState>("state");
    if (!state || isExpired(state.expiresAt)) return jsonResponse({ error: "upload_expired" }, 410);
    if (path === "/status" && request.method === "GET") return jsonResponse({ nextOffset: String(state.nextOffset), complete: !!state.url, videoId: state.videoId, url: state.url });
    if (path === "/cancel" && request.method === "POST") { await this.ctx.storage.deleteAll(); return jsonResponse({ cancelled: true }); }
    if (path !== "/chunk" || request.method !== "PUT") return jsonResponse({ error: "not_found" }, 404);
    const range = parseContentRange(request.headers.get("content-range"));
    const length = Number(request.headers.get("content-length"));
    const maxChunk = Number(request.headers.get("x-max-chunk"));
    const decision = classifyChunk(range, state.nextOffset, state.total, length, maxChunk);
    if (decision === "invalid" || !range) return jsonResponse({ error: "invalid_chunk" }, 400);
    if (decision === "replay") return jsonResponse({ nextOffset: String(state.nextOffset), replayed: true }, 308);
    if (decision === "gap") return jsonResponse({ error: "non_sequential_chunk", nextOffset: String(state.nextOffset) }, 409);
    const upstream = await fetch(state.uploadUrl, { method: "PUT", headers: { authorization: `Bearer ${request.headers.get("x-access-token")}`,
      "content-type": "video/mp4", "content-length": String(length), "content-range": request.headers.get("content-range")! }, body: request.body });
    if (upstream.status === 308) {
      state.nextOffset = nextOffsetFromRange(upstream.headers.get("range"), range.end + 1);
      await this.ctx.storage.put("state", state); return jsonResponse({ nextOffset: String(state.nextOffset) }, 308);
    }
    if (upstream.status === 200 || upstream.status === 201) {
      const result = await upstream.json<{ id?: string }>(); state.nextOffset = state.total; state.videoId = result.id;
      state.url = result.id ? `https://youtu.be/${result.id}` : undefined; await this.ctx.storage.put("state", state);
      return jsonResponse({ videoId: state.videoId, url: state.url }, upstream.status);
    }
    return jsonResponse({ error: "youtube_chunk_failed", upstreamStatus: upstream.status }, upstream.status >= 500 ? 503 : 502);
  }
  async alarm() { await this.ctx.storage.deleteAll(); }
}

export class DailyLimiter extends DurableObject<Env> {
  async fetch(request: Request): Promise<Response> {
    if (request.method !== "POST") return jsonResponse({ error: "method" }, 405);
    const day = new Date().toISOString().slice(0, 10); const ip = request.headers.get("x-ip") ?? "unknown";
    const perIpLimit = Number(request.headers.get("x-per-ip")), globalLimit = Number(request.headers.get("x-global"));
    const globalKey = `global:${day}`, ipKey = `ip:${day}:${ip}`;
    const [globalCount = 0, ipCount = 0] = await Promise.all([this.ctx.storage.get<number>(globalKey), this.ctx.storage.get<number>(ipKey)]);
    if (!canConsume(ipCount, globalCount, perIpLimit, globalLimit)) return jsonResponse({ allowed: false }, 429);
    await this.ctx.storage.put({ [globalKey]: globalCount + 1, [ipKey]: ipCount + 1 });
    return jsonResponse({ allowed: true });
  }
}
