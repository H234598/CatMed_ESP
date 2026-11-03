# ACME 1.1.1 requests the legacy unnamespaced component name. Forward that
# dependency to the current official registry component without vendoring it.
idf_component_get_property(arduinojson_component_dir bblanchon__arduinojson COMPONENT_DIR)
idf_component_register(
    INCLUDE_DIRS "${arduinojson_component_dir}/src"
    REQUIRES bblanchon__arduinojson)
