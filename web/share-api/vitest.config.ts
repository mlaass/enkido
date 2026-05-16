import { defineWorkersConfig } from '@cloudflare/vitest-pool-workers/config';

export default defineWorkersConfig({
	test: {
		poolOptions: {
			workers: {
				wrangler: { configPath: './wrangler.toml' },
				miniflare: {
					compatibilityDate: '2024-09-23',
					compatibilityFlags: ['nodejs_compat'],
					d1Databases: ['DB'],
					bindings: {
						ALLOW_ORIGIN: 'http://localhost:5173',
						ALLOW_ORIGIN_DEV_PATTERN: '^http://localhost:\\d+$',
						LIVE_ORIGIN: 'http://localhost:5173'
					}
				}
			}
		}
	}
});
