# Patch a build-local copy; keep the managed dependency unchanged.
idf_component_get_property(tof_driver_dir pkolt__vl53l0x COMPONENT_DIR)
idf_component_get_property(tof_driver_lib pkolt__vl53l0x COMPONENT_LIB)
set(tof_driver_source "${tof_driver_dir}/src/vl53l0x.c")
file(READ "${tof_driver_source}" tof_driver_code)

function(tof_replace old new)
    string(FIND "${tof_driver_code}" "${old}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "VL53L0X 1.0.0 compatibility patch no longer matches")
    endif()
    string(REPLACE "${old}" "${new}" tof_driver_code "${tof_driver_code}")
    set(tof_driver_code "${tof_driver_code}" PARENT_SCOPE)
endfunction()

# Release the I2C device when handle allocation fails.
tof_replace("if (sensor == NULL) {\n        return ESP_ERR_NO_MEM;"
            "if (sensor == NULL) {\n        i2c_master_bus_rm_device(dev_handle);\n        return ESP_ERR_NO_MEM;")
# Destruction must also work after incomplete initialization.
tof_replace("ESP_RETURN_ON_ERROR(vl53l0x_validate_sensor(sensor), TAG, \"Invalid sensor handle or sensor is not initialized\");"
            "ESP_RETURN_ON_FALSE(sensor, ESP_ERR_INVALID_ARG, TAG, \"Null sensor handle\");")
# Single ranging is synchronous: clear the flag on both success and failure.
tof_replace("ESP_RETURN_ON_ERROR(vl53l0x_check_status(\n        \"VL53L0X_PerformSingleRangingMeasurement\",\n        VL53L0X_PerformSingleRangingMeasurement(&sensor->dev, &raw)), TAG, \"VL53L0X_PerformSingleRangingMeasurement failed\");\n\n    sensor->measuring = false;"
            "esp_err_t ret = vl53l0x_check_status(\n        \"VL53L0X_PerformSingleRangingMeasurement\",\n        VL53L0X_PerformSingleRangingMeasurement(&sensor->dev, &raw));\n    sensor->measuring = false;\n    ESP_RETURN_ON_ERROR(ret, TAG, \"Single ranging failed\");")

# Address reassignment removes the old bus device before creating the new one.
# Clear the stored handle so a failed rebind cannot leave a freed device for destroy().
tof_replace("    dev_handle = NULL;\n\n    const i2c_device_config_t i2c_dev_cfg = {"
            "    dev_handle = NULL;\n    sensor->dev.i2c_dev_handle = NULL;\n\n    const i2c_device_config_t i2c_dev_cfg = {")

set(tof_fixed_source "${CMAKE_CURRENT_BINARY_DIR}/vl53l0x_fixed.c")
file(WRITE "${tof_fixed_source}" "${tof_driver_code}")
get_target_property(tof_driver_sources ${tof_driver_lib} SOURCES)
list(REMOVE_ITEM tof_driver_sources "${tof_driver_source}")
list(APPEND tof_driver_sources "${tof_fixed_source}")
set_property(TARGET ${tof_driver_lib} PROPERTY SOURCES "${tof_driver_sources}")
