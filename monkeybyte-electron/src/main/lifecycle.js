// SPDX-License-Identifier: BSL-1.0

/** Resolve even when a renderer, child process, or network peer stops answering. */
export async function within(operation, milliseconds, fallback = undefined) {
  let timer;
  try {
    return await Promise.race([
      Promise.resolve(operation),
      new Promise((resolve) => {
        timer = setTimeout(() => resolve(fallback), milliseconds);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

/** Close an HTTP server without waiting forever for keep-alive sockets. */
export async function closeHttpServer(server, milliseconds = 1000) {
  if (!server) return true;
  const closed = new Promise((resolve) => server.close(() => resolve(true)));
  server.closeAllConnections?.();
  return within(closed, milliseconds, false);
}
