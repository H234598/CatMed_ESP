# ACME 1.1.1 requests the legacy unnamespaced component name. Forward that
# dependency to the current official registry component without vendoring it.
idf_component_register(
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../managed_components/bblanchon__arduinojson/src"
    REQUIRES bblanchon__arduinojson)
