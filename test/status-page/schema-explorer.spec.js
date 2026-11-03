import {test, expect} from '@playwright/test';

const artifact = 'mapget-schema:sha256:demo';
const rootHash = `#schemas/${encodeURIComponent(artifact)}`;
const schema = {
    mapSchemaId: artifact,
    mapFormat: {name: 'nds.live', version: '', subsystems: [
        {name: 'road', version: '2024.09'}, {name: 'rules', version: '2024.09'},
        {name: 'rules', version: '2024.11'},
    ]},
    consumers: [{mapId: 'Demo/Map', sourceId: 'source-0', role: 'published'}],
    retainedBytes: 123456,
    statistics: {counts: {domains: 100, internedNames: 200}, memory: {'allocated-bytes': 123456}},
    layers: [
        {layerId: 'Road', type: 'Features', schemaId: 10, featureTypes: [
            {name: 'Road', schemaId: 1}, {name: 'Intersection', schemaId: 2},
        ]},
        {layerId: 'Metadata-SpatialExtent', type: 'SourceData', schemaId: 3, featureTypes: []},
    ],
};
const nodes = {
    1: {
        descriptor: {kind: 'Feature', typename: 'Road', open: false},
        fields: [{name: 'properties', $ref: 4}, {name: 'nextRoad', $ref: 1}, {name: 'featureId', $ref: 5, required: true}],
        featureTypes: [{name: 'Road', forwardRelationsKnown: true,
            forwardRelations: [{name: 'nextRoad', targetTypes: ['Road'], locality: 'may-cross-container', complete: true}],
            uniqueIdCompositions: [[{partId: 'roadId', datatype: 'U64', isOptional: false}]],
        }],
    },
    2: {descriptor: {kind: 'Feature', typename: 'Intersection'}, fields: []},
    3: {descriptor: {kind: 'object', typename: 'BoundingBox'}, fields: [{name: 'southWest', $ref: 5}]},
    4: {descriptor: {kind: 'object'}, fields: [{name: 'speedLimitKmh', $ref: 6}, {name: '<img src=x onerror=alert(1)>', $ref: 5}]},
    5: {descriptor: {kind: 'string'}},
    6: {descriptor: {kind: 'int', typename: 'SpeedLimit', numericDomain: {unit: 'km/h', minimum: 0, maximum: 255, scale: 1, complete: true},
        namedValues: [{name: 'UNLIMITED', value: 255, role: 'special', meaning: 'No legal limit'},
            {name: 'EXACT_INT', value: {$mapget: 'uint64', value: '18446744073709551615'}, role: 'constant'}]}},
};

/** Stub only wire data; the script, layout and browser event machinery are the shipped page. */
async function fixture(page) {
    const calls = [];
    let inventory = [schema];
    await page.route(url => url.pathname === '/status-data', route => route.fulfill({json: {}}));
    await page.route('**/schemas', route => route.fulfill({json: {schemas: inventory}}));
    await page.route('**/schema/inspect?**', route => {
        const params = new URL(route.request().url()).searchParams;
        calls.push(Object.fromEntries(params));
        const id = Number(params.get('schemaId'));
        if (params.has('find')) return route.fulfill({json: {
            complete: true, reason: null, discovery: {scanComplete: true},
            items: [{schemaId: 6, kind: 'int', typename: 'SpeedLimit', contexts: [
                {fieldName: 'speedLimitKmh', path: ['properties', 'speedLimitKmh']},
            ]}],
        }});
        return route.fulfill({json: {mapSchemaId: artifact, schemaId: id, ...nodes[id]}});
    });
    return {calls, setInventory: value => { inventory = value; }};
}

async function openSchema(page) {
    await page.goto('/status#schemas');
    await page.getByRole('button', {name: 'Explore', exact: true}).click();
    await expect(page.locator('#schemaDetails')).toBeVisible();
    await expect(page.locator('#schemaTree')).toContainText('Filter immediate members');
}

test('overview exposes every subsystem version and filters by module or consumer', async ({page}) => {
    const {calls} = await fixture(page);
    await page.goto('/status#schemas');
    const table = page.locator('#schemaInventory');
    await expect(table).toContainText('road 2024.09');
    await expect(table).toContainText('rules 2024.09');
    await expect(table).toContainText('rules 2024.11');
    await page.getByLabel('Filter inventory').fill('rules 2024.11');
    await expect(table.getByRole('button', {name: 'Explore'})).toBeVisible();
    await page.getByLabel('Filter inventory').fill('Demo/Map');
    await expect(table.getByRole('button', {name: 'Explore'})).toBeVisible();
    await page.getByLabel('Filter inventory').fill('absent');
    await expect(table).toContainText('No schemas match');
    expect(calls).toHaveLength(0);
});

test('Explore replaces inventory; one root loads and metadata is readable', async ({page}) => {
    const {calls} = await fixture(page);
    await openSchema(page);
    await expect(page).toHaveURL(new RegExp(`${rootHash}$`));
    await expect(page.locator('#schemaOverview')).toBeHidden();
    await expect(page.locator('.history-graph')).toBeHidden();
    await expect(page.locator('#schemaTitle')).toBeFocused();
    await expect(page.locator('#schemaDetails pre')).toHaveCount(0);
    expect(calls.map(call => call.schemaId)).toEqual(['1']);
    await page.getByText('Road: identifier compositions', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('roadId');
    await expect(page.locator('#schemaTree')).toContainText('U64');
    await page.getByText('Road: outgoing relations', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('may-cross-container');
    await page.getByText('nextRoad', {exact: false}).filter({hasText: 'recursive reference'}).first().click();
    expect(calls.map(call => call.schemaId)).toEqual(['1']);
});

test('root navigation has browser history and bookmark restoration', async ({page}) => {
    const {calls} = await fixture(page);
    await openSchema(page);
    await page.getByRole('button', {name: 'Intersection Features'}).click();
    await expect(page.locator('#schemaRootTitle')).toHaveText('Road / Intersection');
    await expect(page).toHaveURL(new RegExp(`${rootHash}/Road/2$`));
    await page.reload();
    await expect(page.locator('#schemaRootTitle')).toHaveText('Road / Intersection');
    // Restoring a deep link must not briefly issue work for the default root.
    expect(calls.filter(call => call.schemaId === '1')).toHaveLength(1);
    await page.getByRole('button', {name: 'Back to schemas'}).click();
    await expect(page.locator('#schemaOverview')).toBeVisible();
    await expect(page.locator('.history-graph')).toBeVisible();
    await page.goBack();
    await expect(page.locator('#schemaRootTitle')).toHaveText('Road / Intersection');
    await expect(page.locator('#schemaOverview')).toBeHidden();
    await page.getByRole('tab', {name: 'Memory', exact: true}).click();
    await expect(page.locator('.history-graph')).toBeVisible();
});

test('navigation and immediate-member filters do not prefetch descendants', async ({page}) => {
    const {calls} = await fixture(page);
    await openSchema(page);
    await page.getByLabel('Filter layers or feature types').fill('SpatialExtent');
    await expect(page.locator('#schemaRoots button')).toHaveCount(1);
    await expect(page.locator('#schemaRoots')).toContainText('Root');
    await page.getByLabel('Filter immediate members of Road', {exact: true}).fill('properties');
    await expect(page.locator('#schemaTree')).toContainText('1 of 3 immediate members');
    await expect(page.locator('.schema-branch[data-schema-id="5"]')).toBeHidden();
    expect(calls.map(call => call.schemaId)).toEqual(['1']);
});

test('descendant Find uses the shared API, then opens typed values without JSON', async ({page}) => {
    const {calls} = await fixture(page);
    await openSchema(page);
    await page.getByLabel('Find in root', {exact: true}).fill('speed limit');
    await page.getByRole('button', {name: 'Find', exact: true}).click();
    await expect(page.locator('#schemaSearchStatus')).toContainText('1 matching domain');
    expect(calls.at(-1)).toEqual({mapSchemaId: artifact, layerId: 'Road', schemaId: '1', find: 'speed limit'});
    await expect(page.locator('#schemaTree')).toContainText('properties / speedLimitKmh');
    await page.locator('.schema-branch[data-schema-id="6"] > summary').click();
    await page.getByText('Numeric encoding and units', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('km/h');
    await page.getByText('Named values (2)', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('18446744073709551615');
    await expect(page.locator('#schemaDetails pre')).toHaveCount(0);
    await page.getByRole('button', {name: 'Show root'}).click();
    await expect(page.locator('#schemaTree')).toContainText('Filter immediate members');
    await expect(page.locator('#schemaFind')).toHaveValue('');
});

test('partial empty search never claims absence', async ({page}) => {
    await fixture(page);
    await page.route('**/schema/inspect?**find=**', route => route.fulfill({json: {
        items: [], complete: false, reason: 'work_limit', discovery: {scanComplete: false},
    }}));
    await openSchema(page);
    await page.getByLabel('Find in root', {exact: true}).fill('speed');
    await page.getByRole('button', {name: 'Find', exact: true}).click();
    await expect(page.locator('#schemaSearchStatus')).toContainText('Partial results (work_limit)');
    await expect(page.locator('#schemaTree')).toContainText('not proof of absence');
});

test('failed domains retry once and hostile names remain literal text', async ({page}) => {
    await fixture(page);
    let failures = 1;
    await page.route('**/schema/inspect?**schemaId=4', route => failures-- > 0
        ? route.fulfill({status: 503, body: 'Temporary failure'}) : route.fallback());
    await openSchema(page);
    await page.locator('.schema-branch[data-schema-id="4"] > summary').click();
    await expect(page.getByRole('button', {name: 'Retry', exact: true})).toBeVisible();
    await page.getByRole('button', {name: 'Retry', exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('speedLimitKmh');
    await expect(page.locator('#schemaTree')).toContainText('<img src=x onerror=alert(1)>');
    await expect(page.locator('#schemaTree img')).toHaveCount(0);
    await expect(page.getByRole('button', {name: 'Retry', exact: true})).toHaveCount(0);
});

test('a late search cannot populate a different selected root', async ({page}) => {
    await fixture(page);
    let finish;
    const gate = new Promise(resolve => { finish = resolve; });
    await page.route('**/schema/inspect?**find=**', async route => {
        await gate;
        await route.fulfill({json: {items: [{schemaId: 99, typename: 'STALE SEARCH RESULT'}], complete: true}}).catch(() => {});
    });
    await openSchema(page);
    await page.getByLabel('Find in root', {exact: true}).fill('slow');
    const requested = page.waitForRequest(request => request.url().includes('find=slow'));
    await page.getByRole('button', {name: 'Find', exact: true}).click();
    await requested;
    await page.getByRole('button', {name: 'Intersection Features'}).click();
    finish();
    await expect(page.locator('#schemaRootTitle')).toHaveText('Road / Intersection');
    await expect(page.locator('#schemaTree')).not.toContainText('STALE SEARCH RESULT');
    await expect(page.locator('#schemaTree')).toContainText('Intersection');
    await expect(page.locator('#schemaFindSubmit')).toBeEnabled();
});

test('removal from authorized inventory clears the selected graph', async ({page}) => {
    const {setInventory} = await fixture(page);
    await openSchema(page);
    setInventory([]);
    await page.getByRole('button', {name: 'Refresh', exact: true}).click();
    await expect(page.locator('#schemaDetails')).toBeHidden();
    await expect(page.locator('#schemaSummary')).toBeEmpty();
    await expect(page.locator('#schemaTree')).toBeEmpty();
    await expect(page.locator('#schemaStatus')).toContainText('no longer available');
});

test('narrow screens keep navigation and explorer within the viewport in both themes', async ({page}) => {
    await page.setViewportSize({width: 390, height: 844});
    const {setInventory} = await fixture(page);
    const wide = structuredClone(schema);
    wide.layers[0].layerId = 'NDS.Classic.VeryLongUnbrokenLayerIdentifier'.repeat(3);
    setInventory([wide]);
    await openSchema(page);
    for (const theme of ['dark', 'light']) {
        await page.locator('#themeSelect').selectOption(theme);
        expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
        await expect(page.locator('#schemaBack')).toBeVisible();
        await expect(page.locator('#schemaOverview')).toBeHidden();
    }
});

test('a removed or malformed bookmark does not request an arbitrary domain', async ({page}) => {
    const {calls} = await fixture(page);
    await page.goto('/status#schemas/unavailable/Road/1');
    await expect(page.locator('#schemaStatus')).toContainText('no longer available');
    await expect(page.locator('#schemaOverview')).toBeVisible();
    expect(calls).toHaveLength(0);
    await page.goto('/status#schemas/%ZZ');
    await expect(page.locator('#schemaOverview')).toBeVisible();
    expect(calls).toHaveLength(0);
});
