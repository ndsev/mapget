/** Serve the exact embedded HTML for DOM tests; API responses are test-owned fixtures. */
import {createServer} from 'node:http';
import {readFileSync} from 'node:fs';

const source = new URL('../../libs/http-service/src/status-page.cpp', import.meta.url);
createServer((request, response) => {
    if (request.url !== '/status') {
        response.writeHead(404).end();
        return;
    }
    const html = [...readFileSync(source, 'utf8').matchAll(/R"STATUS\(([\s\S]*?)\)STATUS"/g)]
        .map(match => match[1]).join('');
    response.writeHead(200, {'Content-Type': 'text/html', 'Cache-Control': 'no-store'}).end(html);
}).listen(18982, '127.0.0.1');
