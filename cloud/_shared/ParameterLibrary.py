"""Cumulocity device-parameter keywords for the tedge-dot cloud e2e suites.

Manages Digital Twin Manager (DTM) property definitions — the tenant-side declaration of
which parameter sets a device exposes. The definitions themselves are rendered by
`tedge-dot describe` so the suite posts exactly what a tenant admin would.

Auth comes from the same environment variables as robotframework-c8y
(C8Y_BASEURL, C8Y_USER, C8Y_PASSWORD, C8Y_TENANT), via c8y_test_core.
"""

import json
import logging
from typing import Any, Dict, List, Union

from c8y_test_core.c8y import CustomCumulocityApp
from dotenv import load_dotenv
from robot.api.deco import keyword, library

logger = logging.getLogger(__name__)

DTM_PROPERTIES = "/service/dtm/definitions/properties"


@library(scope="SUITE", auto_keywords=False)
class ParameterLibrary:
    """Robot keywords for Cumulocity device parameters (DTM definitions)."""

    def __init__(self):
        load_dotenv()
        try:
            self.c8y = CustomCumulocityApp()
        except Exception as ex:  # allow --dryrun / import without tenant credentials
            logger.warning("Could not load Cumulocity API client: %s", ex)
            self.c8y = None

    @keyword("Ensure DTM Property Definition")
    def ensure_dtm_property_definition(
        self, definition: Union[str, Dict[str, Any]], replace: bool = False
    ) -> Dict[str, Any]:
        """Create the DTM property definition (as rendered by `tedge-dot describe`) unless it
        exists. With replace=True an existing definition is deleted and re-created so schema
        changes in the connector config reach the tenant. Returns the definition in place."""
        body = json.loads(definition) if isinstance(definition, str) else dict(definition)
        identifier = body["identifier"]
        # The per-identifier GET of the DTM service is context-scoped and does not resolve
        # a definition by identifier alone; the list endpoint is the reliable existence check.
        existing = self._find_definition(identifier)
        if existing and not replace:
            logger.info("DTM definition %s already exists", identifier)
            return existing
        if existing:
            contexts = ",".join(existing.get("contexts") or body.get("contexts", []))
            self.c8y.delete(f"{DTM_PROPERTIES}/{identifier}?contexts={contexts}")
        created = self.c8y.post(DTM_PROPERTIES, json=body)
        logger.info("DTM definition %s created", identifier)
        return created

    def _find_definition(self, identifier: str):
        page = self.c8y.get(DTM_PROPERTIES, params={"pageSize": "2000"})
        for d in page.get("definitions", []):
            if d.get("identifier") == identifier:
                return d
        return None

    @keyword("DTM Property Definitions Should Contain")
    def dtm_property_definitions_should_contain(self, identifier: str) -> Dict[str, Any]:
        """Assert a DTM property definition with the identifier is listed."""
        found = self._find_definition(identifier)
        if found is None:
            raise AssertionError(f"no DTM property definition named {identifier}")
        return found
