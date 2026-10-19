# Lensfun database snapshot

Copyright the Lensfun database contributors. License: Creative Commons Attribution-ShareAlike 3.0 (included).

Source: https://github.com/lensfun/lensfun/tree/43f9e001ab66c4fcdd4f400463f13b72f94b2288/data/db
Retrieved October 19, 2026. Source archive: https://github.com/lensfun/lensfun/archive/43f9e001ab66c4fcdd4f400463f13b72f94b2288.zip

`database.xml` combines that revision's XML files, sorted by filename, into one schema-1 database for Lensfun 0.3.4. Changes follow upstream's `tools/update_database/xml_converter.py` From2To1: remove unsupported ACM calibration elements and convert `real-focal` attributes into `real-focal-length` elements. XML comments/formatting are regenerated. This modified database remains CC BY-SA 3.0. The application and its code have separate licenses.

The snapshot is built into a compressed Qt resource. No network request or database installation is needed when importing photographs. Missing/ambiguous matches are not replaced with approximate lens identities. Runtime calibration interpolation is performed by the dynamically linked Lensfun library, not a custom approximation.
