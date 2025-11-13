# Mapget User Guide

- Setup Guide (mapget-setup.md)
  - Python Installation
  - GeoJson DataSource
    (TODO: Migrate from mapget-live-cpp)
  - GridStream DataSource
    (TODO: Migrate DevDataSource from mapviewer repo)
- REST API Guide (mapget-api.md)
- Caching Guide (TTL, Clear Cache, in-memory vs sqlite vs no cache) (mapget-cache.md)
- Layered Data Model (mapget-model.md)
  - Tiling Scheme
  - TileFeatureLayer
    - API (Coarse - focus on classes and relationships internally and externally towards simfil, UML diagram)
    - GeoJSON Representation
    - Geographic simfil model/language extensions
  - TileSourceDataLayer
    - API (Coarse - focus on classes and relationships internally and externally towards simfil, UML diagram)
    - GeoJSON Representation
  - DataSource/Map/Layer Metadata
  - TileLayerStream
    - Shared String Dictionaries per node
    - Description of binary protocol
