# Netlify Deployment Setup

Automatic deployment via GitHub Actions to Netlify.

## How It Works

- **Push to `master`** → Deploys to dev Netlify site (draft deploy)
- **Push tag `v*`** → Deploys to production Netlify site

The workflow builds WASM from source using Emscripten, then builds the web app with Bun.

## One-Time Setup

### 1. Create Netlify Sites

1. Go to https://app.netlify.com
2. Click "Add new site" → "Deploy manually"
3. Create two sites:
   - `enkido-dev` (for dev/preview deploys)
   - `enkido` (for production releases)
4. For each site, go to **Site Settings → General** and copy the **Site ID**

### 2. Generate Netlify Auth Token

1. Go to https://app.netlify.com/user/applications
2. Under "Personal access tokens", click "New access token"
3. Give it a name (e.g., "enkido-github-actions")
4. Copy the token (you won't see it again)

### 3. Add GitHub Secrets

1. Go to your GitHub repo → **Settings → Secrets and variables → Actions**
2. Add these secrets:

| Secret Name | Value |
|-------------|-------|
| `NETLIFY_AUTH_TOKEN` | Your personal access token from step 2 |
| `NETLIFY_SITE_ID_DEV` | Site ID of your dev site |
| `NETLIFY_SITE_ID_PROD` | Site ID of your production site |

## Triggering Deploys

**Dev deploy:**
```bash
git push origin master
```

**Production deploy:**
```bash
git tag v1.0.0
git push origin v1.0.0
```

## Configuration Files

- `.github/workflows/deploy.yml` - GitHub Actions workflow
- `web/netlify.toml` - Netlify headers and routing

## Important Headers

The `netlify.toml` sets required headers for SharedArrayBuffer (needed for AudioWorklet):
- `Cross-Origin-Opener-Policy: same-origin`
- `Cross-Origin-Embedder-Policy: require-corp`

Without these headers, the audio engine will not function.

## Troubleshooting

**Build fails at WASM step:**
- Check Emscripten setup in the workflow
- Verify `web/wasm/CMakeLists.txt` is valid

**Audio doesn't work on deployed site:**
- Check browser console for SharedArrayBuffer errors
- Verify headers are being applied (Network tab → Response Headers)

**Deploy step fails:**
- Verify all three GitHub secrets are set correctly
- Check Netlify auth token hasn't expired
