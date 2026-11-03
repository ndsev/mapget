import {defineConfig} from '@playwright/test';

export default defineConfig({
    testDir: '.',
    testMatch: '*.spec.js',
    workers: 1,
    use: {
        baseURL: 'http://127.0.0.1:18982',
        viewport: {width: 1440, height: 1000},
        trace: 'retain-on-failure',
        screenshot: 'only-on-failure',
    },
    webServer: {
        command: 'node serve.mjs',
        url: 'http://127.0.0.1:18982/status',
        reuseExistingServer: false,
    },
});
