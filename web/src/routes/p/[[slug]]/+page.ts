// Inline-URL viewer route. Client-only — adapter-static, no SSR/prerender.
// The hash fragment carrying `code=...` is not visible to the server anyway,
// so SSR would have nothing useful to render.
export const prerender = false;
export const ssr = false;
