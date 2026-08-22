export type LensError = { code: string; message: string; retryable?: boolean };
export type Result<T> = { ok: true; value: T } | { ok: false; error: LensError };
export type Photon = {
  sequence: number; id: string; ray: string; parent?: string | null; kind: string;
  schema: string; payload: unknown; epoch: number; committed_at_ms: number;
};
export type PhotonWindow = { photons: Photon[] };
export type Act = {
  id: string; ray: string; kind: string; schema: string; parameters: Record<string, unknown>;
  target: string; epoch: number; generation: number; risk: string; approved: boolean;
};
export type RefractionResult = { status: "passed" | "completed" | "rejected" | "failed"; detail?: string };
export declare const ok: <T>(value: T) => Result<T>;
export declare const err: <T = never>(code: string, message: string, retryable?: boolean) => Result<T>;
export declare const defineLens: <T>(lens: T) => Readonly<T>;
export declare function actPattern<T>(kind: string, schema?: string): {
  kind: string; schema: string; match(act: Act): Result<T>;
};
export declare class SurfaceBuilder {
  contributions: unknown[]; proposals: Act[];
  constructor(lens: string);
  add(channel: string, key: string, value: unknown, priority?: number): Result<void>;
  propose(act: Act): Result<void>;
  readonly model: {
    addTool(tool: { name: string; description: string; argumentsSchema: string }): Result<void>;
    addContext(key: string, value: unknown): Result<void>;
  };
  readonly ui: { add(key: string, value: unknown, priority?: number): Result<void> };
}
export declare class RefractionBeam {
  act: Act; signal: AbortSignal; drafts: unknown[]; logs: unknown[];
  emitter: {
    emit(kind: string, schema: string, payload: unknown): Promise<Result<{ id: string }>>;
    toolResult(act: Act, toolName: string, payload: Record<string, unknown>): Promise<Result<{ id: string }>>;
  };
  log(level: string, message: string, fields?: Record<string, unknown>): void;
}
export declare const passed: () => RefractionResult;
export declare const completed: (detail?: string) => RefractionResult;
export declare const rejected: (detail: string) => RefractionResult;

