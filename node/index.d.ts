export function extensionPath(): string;
export function load<T extends { loadExtension(path: string): unknown }>(db: T): T;
export const version: string;
