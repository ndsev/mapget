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
    statistics: {counts: {types: 100, physicalTypes: 80, queryContexts: 20, internedNames: 200},
        limits: {typeId: 65535}, memory: {'allocated-bytes': 123456}},
    layers: [
        {layerId: 'Road', type: 'Features', category: 'Features', typeId: 10, featureTypes: [
            {name: 'Road', typeId: 1}, {name: 'Intersection', typeId: 2},
        ]},
        {layerId: 'Raw-Header', type: 'SourceData', category: 'SourceData', typeId: 5, featureTypes: []},
        {layerId: 'Metadata-SpatialExtent', type: 'SourceData', category: 'Metadata', typeId: 3,
            open: true, reachabilityComplete: false, featureTypes: []},
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
    6: {descriptor: {kind: 'integer', typename: 'SpeedLimit', numericDomain: {unit: 'km/h', minimum: 0, maximum: 255, scale: 1, complete: true},
        namedValues: [{name: 'UNLIMITED', value: 255, role: 'special', meaning: 'No legal limit'},
            {name: 'EXACT_INT', value: 'EXACT_INT', rawValue: '18446744073709551615', role: 'constant'}]}},
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
        const id = Number(params.get('typeId'));
        if (params.has('find')) return route.fulfill({json: {
            complete: true, reason: null, discovery: {scanComplete: true},
            items: [{typeId: 6, kind: 'integer', typename: 'SpeedLimit', contexts: [
                {fieldName: 'speedLimitKmh', path: ['properties', 'speedLimitKmh']},
            ]}],
        }});
        return route.fulfill({json: {mapSchemaId: artifact, typeId: id, ...nodes[id]}});
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
    await expect(table.getByRole('columnheader', {name: 'Physical types'})).toBeVisible();
    await expect(table.getByRole('columnheader', {name: 'Query contexts'})).toBeVisible();
    await expect(table).toContainText('100 / 65,535');
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
    expect(calls.map(call => call.typeId)).toEqual(['1']);
    await page.getByText('Road: identifier compositions', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('roadId');
    await expect(page.locator('#schemaTree')).toContainText('U64');
    await page.getByText('Road: outgoing relations', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('may-cross-container');
    await page.getByText('nextRoad', {exact: false}).filter({hasText: 'recursive reference'}).first().click();
    expect(calls.map(call => call.typeId)).toEqual(['1']);
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
    expect(calls.filter(call => call.typeId === '1')).toHaveLength(1);
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
    await page.getByLabel('Filter layers or feature types').fill('Intersection');
    await expect(page.locator('#schemaRoots button')).toHaveCount(1);
    await expect(page.locator('#schemaRoots')).toContainText('Intersection');
    await page.getByLabel('Filter immediate members of Road', {exact: true}).fill('properties');
    await expect(page.locator('#schemaTree')).toContainText('1 of 3 immediate members');
    await expect(page.locator('.schema-branch[data-type-id="5"]')).toBeHidden();
    expect(calls.map(call => call.typeId)).toEqual(['1']);
});

test('descendant Find uses the shared API, then opens typed values without JSON', async ({page}) => {
    const {calls} = await fixture(page);
    await openSchema(page);
    await page.getByLabel('Find in root', {exact: true}).fill('speed limit');
    await page.getByRole('button', {name: 'Find', exact: true}).click();
    await expect(page.locator('#schemaSearchStatus')).toContainText('1 matching type');
    expect(calls.at(-1)).toEqual({mapSchemaId: artifact, layerId: 'Road', typeId: '1', find: 'speed limit'});
    await expect(page.locator('#schemaTree')).toContainText('properties / speedLimitKmh');
    await page.locator('.schema-branch[data-type-id="6"] > summary').click();
    await page.getByText('Numeric encoding and units', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('km/h');
    await page.getByText('Named values (2)', {exact: true}).click();
    await expect(page.locator('#schemaTree').getByRole('columnheader', {name: 'Raw ordinal', exact: true})).toBeVisible();
    await expect(page.locator('#schemaTree')).toContainText('18446744073709551615');
    await expect(page.locator('#schemaDetails pre')).toHaveCount(0);
    await page.getByRole('button', {name: 'Show root'}).click();
    await expect(page.locator('#schemaTree')).toContainText('Filter immediate members');
    await expect(page.locator('#schemaFind')).toHaveValue('');
});

/** Native finder summaries carry counts and matches, not the full descriptor's value arrays. */
test('Find renders enum and named-value summaries before loading their descriptors', async ({page}) => {
    const {calls} = await fixture(page);
    await page.route('**/schema/inspect?**find=**', route => route.fulfill({json: {
        complete: true, items: [
            {typeId: 7, kind: 'string', typename: 'AdasAccuracy',
                enum: {symbolCount: 4, literalCount: 0, symbolSemantics: 'string_literals',
                    preview: ['UNKNOWN', 'LOW', 'MIDDLE', 'HIGH'], matchingSymbols: []},
                namedValues: {count: 4, matchingCount: 0, matchingNames: []},
                contexts: [{fieldName: 'adasAccuracy', path: ['properties', 'ADAS_ACCURACY', 'adasAccuracy']}]},
            {typeId: 6, kind: 'integer', typename: 'SpeedLimit',
                namedValues: {count: 2, matchingCount: 1, matchingNames: ['UNLIMITED']},
                contexts: [{fieldName: 'speedLimitKmh', path: ['properties', 'speedLimitKmh']}]},
        ],
    }}));
    await openSchema(page);
    await page.getByLabel('Find in root', {exact: true}).fill('accuracy');
    await page.getByRole('button', {name: 'Find', exact: true}).click();
    await expect(page.locator('#schemaSearchStatus')).toContainText('2 matching type(s)');
    await expect(page.locator('[data-type-id="7"] > summary')).toContainText('AdasAccuracy · string · 4 enum values · 4 named values');
    await expect(page.locator('[data-type-id="6"] > summary')).toContainText('SpeedLimit · integer · 2 named: UNLIMITED, …');
    expect(calls.map(call => call.typeId)).toEqual(['1']);
    await page.locator('[data-type-id="6"] > summary').click();
    await page.getByText('Named values (2)', {exact: true}).click();
    await expect(page.locator('#schemaTree')).toContainText('18446744073709551615');
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

test('failed types retry once and hostile names remain literal text', async ({page}) => {
    await fixture(page);
    let failures = 1;
    await page.route('**/schema/inspect?**typeId=4', route => failures-- > 0
        ? route.fulfill({status: 503, body: 'Temporary failure'}) : route.fallback());
    await openSchema(page);
    await page.locator('.schema-branch[data-type-id="4"] > summary').click();
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
        await route.fulfill({json: {items: [{typeId: 99, typename: 'STALE SEARCH RESULT'}], complete: true}}).catch(() => {});
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

test('a removed or malformed bookmark does not request an arbitrary type', async ({page}) => {
    const {calls} = await fixture(page);
    await page.goto('/status#schemas/unavailable/Road/1');
    await expect(page.locator('#schemaStatus')).toContainText('no longer available');
    await expect(page.locator('#schemaOverview')).toBeVisible();
    expect(calls).toHaveLength(0);
    await page.goto('/status#schemas/%ZZ');
    await expect(page.locator('#schemaOverview')).toBeVisible();
    expect(calls).toHaveLength(0);
});

/** Categories select real published roots; naming-based metadata remains explicitly identified. */
test('Features is the default category; raw and metadata roots remain discoverable', async ({page}) => {
    const {calls} = await fixture(page);
    await openSchema(page);
    await expect(page.getByLabel('Layer category', {exact: true})).toHaveValue('Features');
    await expect(page.locator('#schemaRoots')).not.toContainText('Metadata-SpatialExtent');
    await expect(page.locator('#schemaRoots')).not.toContainText('Raw-Header');
    await page.getByLabel('Layer category', {exact: true}).selectOption('SourceData');
    await expect(page.locator('#schemaRootTitle')).toHaveText('Raw-Header / Root');
    await page.getByLabel('Layer category', {exact: true}).selectOption('Metadata');
    await expect(page.locator('#schemaRootTitle')).toHaveText('Metadata-SpatialExtent / Root');
    await expect(page.locator('#schemaRoots')).toContainText('Partial contract');
    await expect(page.locator('.schema-navigation')).toContainText('naming convention');
    await page.reload();
    await expect(page.getByLabel('Layer category', {exact: true})).toHaveValue('Metadata');
    await expect(page.locator('#schemaRootTitle')).toHaveText('Metadata-SpatialExtent / Root');
    expect(calls.every(call => call.typeId && !Object.hasOwn(call, 'schemaId'))).toBe(true);
});

test('an empty category remains selected after inventory refresh', async ({page}) => {
    const {calls, setInventory} = await fixture(page);
    setInventory([{...schema, layers: [schema.layers[0]]}]);
    await openSchema(page);
    await page.getByLabel('Layer category', {exact: true}).selectOption('Metadata');
    await expect(page.locator('#schemaRootTitle')).toHaveText('No declared roots');
    await page.getByRole('button', {name: 'Refresh', exact: true}).click();
    await expect(page.getByLabel('Layer category', {exact: true})).toHaveValue('Metadata');
    await expect(page.locator('#schemaRootTitle')).toHaveText('No declared roots');
    expect(calls.map(call => call.typeId)).toEqual(['1']);
});

/** Equal labels cannot authorize hiding distinct graph nodes or oneOf alternatives. */
test('nullable facts are concise while genuine choices retain both type identities', async ({page}) => {
    await fixture(page);
    await page.route('**/schema/inspect?**typeId=1', route => route.fulfill({json: {
        descriptor: {kind: 'Feature', typename: 'Road'},
        fields: [
            {name: 'geometry', $ref: 7, descriptor: {kind: 'object', typename: 'Geometry', nullable: true}},
            {name: 'payload', $ref: 8, descriptor: {kind: 'oneOf'}},
        ],
    }}));
    await page.route('**/schema/inspect?**typeId=7', route => route.fulfill({json: {
        descriptor: {kind: 'object', typename: 'Geometry', nullable: true}, fields: [],
    }}));
    await page.route('**/schema/inspect?**typeId=8', route => route.fulfill({json: {
        descriptor: {kind: 'oneOf'}, alternatives: [
            {$ref: 9, descriptor: {kind: 'object', typename: 'Payload'}},
            {$ref: 10, descriptor: {kind: 'object', typename: 'Payload'}},
        ],
    }}));
    await openSchema(page);
    await expect(page.locator('[data-type-id="7"] > summary')).toContainText('Geometry · object · nullable');
    await page.locator('[data-type-id="7"] > summary').click();
    await expect(page.locator('[data-type-id="7"] .schema-branch')).toHaveCount(0);
    await page.locator('[data-type-id="8"] > summary').click();
    await expect(page.locator('[data-type-id="9"] > summary')).toContainText('Choice 1: Payload');
    await expect(page.locator('[data-type-id="10"] > summary')).toContainText('Choice 2: Payload');
    await expect(page.locator('[data-type-id="8"] .schema-branch')).toHaveCount(2);
});

/** Native facts explain a genuine symbolic-value/unknown-code choice before expansion. */
test('equal type names retain scalar kind and bounded named-value distinctions', async ({page}) => {
    const {calls} = await fixture(page);
    await page.route('**/schema/inspect?**typeId=1', route => route.fulfill({json: {
        descriptor: {kind: 'Feature', typename: 'Road'},
        fields: [{name: 'accuracy', $ref: 8, descriptor: {kind: 'oneOf'}}],
    }}));
    await page.route('**/schema/inspect?**typeId=8', route => route.fulfill({json: {
        descriptor: {kind: 'oneOf'}, alternatives: [
            {$ref: 9, descriptor: {kind: 'string', typename: 'Accuracy', enumCount: 4,
                namedValueCount: 4, namedValueNames: ['LOW', 'MEDIUM', 'HIGH']}},
            {$ref: 10, descriptor: {kind: 'integer', typename: 'Accuracy'}},
        ],
    }}));
    await openSchema(page);
    await page.locator('[data-type-id="8"] > summary').click();
    await expect(page.locator('[data-type-id="9"] > summary')).toContainText('Accuracy · string · 4 enum values · 4 named: LOW, MEDIUM, HIGH, …');
    await expect(page.locator('[data-type-id="10"] > summary')).toContainText('Accuracy · integer');
    await expect(page.locator('[data-type-id="8"] .schema-branch')).toHaveCount(2);
    expect(calls.some(call => ['9', '10'].includes(call.typeId))).toBe(false);
});

/** Raw constraints and conversion metadata distinguish choices without pretending they are display units. */
test('numeric alternatives preview exact ranges, units and encoding without child requests', async ({page}) => {
    const {calls} = await fixture(page);
    await page.route('**/schema/inspect?**typeId=1', route => route.fulfill({json: {
        descriptor: {kind: 'Feature', typename: 'Road'},
        fields: [{name: 'distance', $ref: 8, descriptor: {kind: 'oneOf'}}],
    }}));
    await page.route('**/schema/inspect?**typeId=8', route => route.fulfill({json: {
        descriptor: {kind: 'oneOf'}, alternatives: [
            {$ref: 9, descriptor: {kind: 'integer', typename: 'Distance', numericDomain: {
                minimum: 0, maximum: {$mapget: 'uint64', value: '18446744073709551615'},
                ordinaryRangeCount: 3, ordinaryRanges: [{min: 0, max: 9}, {min: 20, max: 29}],
                unit: 'cm', encoding: 'uint64', scale: 0.1, offset: 2, complete: true,
            }}},
            {$ref: 10, descriptor: {kind: 'integer', typename: 'Distance', numericDomain: {
                minimum: 0, maximum: 255, unit: 'm', encoding: 'uint8', scale: 1, offset: 0, complete: false,
            }}},
        ],
    }}));
    await openSchema(page);
    await page.locator('[data-type-id="8"] > summary').click();
    const first = page.locator('[data-type-id="9"] > summary');
    await expect(first).toContainText('Distance · integer · raw 0…18446744073709551615');
    await expect(first).toContainText('ordinary raw 0…9, 20…29, … (3 ranges)');
    await expect(first).toContainText('unit cm · encoding uint64 · scale 0.1 · offset 2');
    await expect(page.locator('[data-type-id="10"] > summary')).toContainText('raw 0…255 · unit m · encoding uint8 · partial numeric metadata');
    await expect(page.locator('[data-type-id="8"] .schema-branch')).toHaveCount(2);
    expect(calls.some(call => ['9', '10'].includes(call.typeId))).toBe(false);
});
