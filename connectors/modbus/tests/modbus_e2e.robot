*** Settings ***
Documentation       End-to-end tests for the Rust tedge-dot against a real Modbus
...                 simulator (pymodbus). The connector reads the simulator and publishes raw
...                 samples + status to a local MQTT broker; these tests assert on that output.
...                 No cloud (Cumulocity) is involved.
...
...                 Run via:  just test-e2e   (brings the Docker stack up/down automatically)

Resource            ../../_shared/stack.resource
Library             Collections

Suite Setup         Setup OT Stack    modbus
Suite Teardown      Teardown OT Stack


*** Variables ***
${DEVICE}               plc1
${PROTOCOL}             modbus
${SERVICE}              tedge-dot

${SAMPLE_PREFIX}        te/device/${DEVICE}/ot/${PROTOCOL}/sample
${CMD_PREFIX}           te/device/${DEVICE}/ot/${PROTOCOL}/cmd/write
${LINK_TOPIC}           te/device/${DEVICE}/ot/${PROTOCOL}/status/link
${CAPS_TOPIC}           te/device/main/service/${SERVICE}/ot/capabilities
${HEALTH_TOPIC}         te/device/main/service/${SERVICE}/status/health
${BATCH_PREFIX}         te/device/${DEVICE}/ot/${PROTOCOL}/cmd/write-batch
${PARAM_CMD_PREFIX}     te/device/${DEVICE}///cmd/parameter_update
${PARAM_TWIN}           te/device/${DEVICE}///twin/${PROTOCOL}_parameters
# The flows container installs thin-edge from the main channel at build time; give it time.
${FLOWS_TIMEOUT}        120

# Generous timeout: the connector waits for the simulator/broker before it starts.
${READY_TIMEOUT}        90
${SAMPLE_TIMEOUT}       15


*** Test Cases ***
Connector Publishes Capability Descriptor
    [Documentation]    The connector advertises its protocol and supported command verbs.
    ${payload}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${protocol}=    Get Json Field    ${payload}    protocol
    Should Be Equal    ${protocol}    modbus
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write

Service Health Is Up
    [Documentation]    The connector publishes a retained service health status of "up".
    ${payload}=    Wait For Retained    ${HEALTH_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    up

Device Link Is Connected
    [Documentation]    The connector reports the Modbus device link as connected.
    ${payload}=    Wait For Retained    ${LINK_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    connected

Reads Uint16 Holding Register
    [Documentation]    Reads a uint16 holding register seeded to 17001 in the simulator.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temp_u16    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${datatype}=    Get Json Field    ${payload}    datatype
    Should Be Equal    ${datatype}    uint16
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    17001

Reads Uint32 Across Two Registers
    [Documentation]    Reads a uint32 value (617001) spanning two registers.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/count_u32    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    617001

Reads Float32 Across Two Registers
    [Documentation]    Reads a float32 value (~404.17) spanning two registers.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/level_f32    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${value}=    Get Json Field    ${payload}    value
    Should Be True    abs(${value} - 404.17) < 0.05

Invalid Register Reports Bad Quality
    [Documentation]    Reading a flagged-invalid address yields a bad-quality sample with an error.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/bad_point    timeout=${SAMPLE_TIMEOUT}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    bad
    ${error}=    Get Json Field    ${payload}    error
    Should Not Be Empty    ${error}

Writes A Coil And Reads It Back
    [Documentation]    A write command sets coil 48 true; the next sample reflects it.
    Publish Message    ${CMD_PREFIX}/coil-1    {"status":"init","point":"coil_rw","value":true}    retain=True
    ${result}=    Wait For Message Containing    ${CMD_PREFIX}/coil-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${point}=    Get Json Field    ${result}    point
    Should Be Equal    ${point}    coil_rw
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/coil_rw    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal    ${value}    ${True}

Writes A Holding Register And Reads It Back
    [Documentation]    A write command sets holding register 3 to 4242; the next sample reflects it.
    ...                 Runs after the uint16 read assertion (the stack is recreated per run).
    Publish Message    ${CMD_PREFIX}/reg-1    {"status":"init","point":"temp_u16","value":4242}    retain=True
    ${result}=    Wait For Message Containing    ${CMD_PREFIX}/reg-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${point}=    Get Json Field    ${result}    point
    Should Be Equal    ${point}    temp_u16
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temp_u16    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    4242


Samples Carry The Point Access
    [Documentation]    Every sample echoes the point's declared access, so flows can tell
    ...                writable points (parameters) apart without reading the config file.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temp_u16    timeout=${SAMPLE_TIMEOUT}
    ${access}=    Get Json Field    ${payload}    access
    Should Be Equal    ${access}    read_write
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/level_f32    timeout=${SAMPLE_TIMEOUT}
    ${access}=    Get Json Field    ${payload}    access
    Should Be Equal    ${access}    read

Capability Descriptor Advertises Write Batch
    [Documentation]    The runtime adds the write-batch verb for every module that implements write.
    ${payload}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write-batch

Write Batch Writes Several Points In One Command
    [Documentation]    One write-batch request writes a register and a coil in order and reports
    ...                a per-point result; the next samples reflect both values.
    Publish Message    ${BATCH_PREFIX}/batch-1
    ...    {"status":"init","writes":[{"point":"temp_u16","value":4243},{"point":"coil_rw","value":true}]}    retain=True
    ${result}=    Wait For Message Containing    ${BATCH_PREFIX}/batch-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    Should Be Equal    ${results}[0][point]    temp_u16
    Should Be Equal    ${results}[0][status]    successful
    Should Be Equal    ${results}[1][point]    coil_rw
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temp_u16    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    4243
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/coil_rw    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal    ${value}    ${True}

Write Batch Stops At The First Failure And Reports What Was Applied
    [Documentation]    A batch with an unknown point fails, but the result lists the write that
    ...                succeeded before it so the requester knows the device state.
    Publish Message    ${BATCH_PREFIX}/batch-2
    ...    {"status":"init","writes":[{"point":"temp_u16","value":17001},{"point":"no_such_point","value":1},{"point":"coil_rw","value":false}]}    retain=True
    ${result}=    Wait For Message Containing    ${BATCH_PREFIX}/batch-2    "status":"failed"    timeout=${SAMPLE_TIMEOUT}
    ${reason}=    Get Json Field    ${result}    reason
    Should Contain    ${reason}    no_such_point
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    Should Be Equal    ${results}[0][status]    successful
    Should Be Equal    ${results}[1][status]    failed
    # the coil after the failing entry was never written
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/coil_rw    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal    ${value}    ${True}

Write Batch Rejects An Empty Request
    Publish Message    ${BATCH_PREFIX}/batch-3    {"status":"init","writes":[]}    retain=True
    ${result}=    Wait For Message Containing    ${BATCH_PREFIX}/batch-3    "status":"failed"    timeout=${SAMPLE_TIMEOUT}
    ${reason}=    Get Json Field    ${result}    reason
    Should Contain    ${reason}    no writes

Flows Register The Device And Advertise The Parameter Capability
    [Documentation]    (flows) ot-registration turns the link status into a child-device
    ...                registration and advertises parameter_update so a cloud mapper routes
    ...                c8y_ParameterUpdate operations to it.
    [Tags]    flows
    ${payload}=    Wait For Retained    te/device/${DEVICE}//    timeout=${FLOWS_TIMEOUT}
    ${type}=    Get Json Field    ${payload}    @type
    Should Be Equal    ${type}    child-device
    Wait For Retained    ${PARAM_CMD_PREFIX}    timeout=${FLOWS_TIMEOUT}

Parameter Twin Follows The Device
    [Documentation]    (flows) ot-parameter-state publishes the writable points of the device as
    ...                one twin fragment per parameter set, fed by the connector's samples.
    [Tags]    flows
    ${payload}=    Wait For Message Containing    ${PARAM_TWIN}    "temp_u16":    timeout=${FLOWS_TIMEOUT}
    ${twin}=    Evaluate    json.loads($payload)    modules=json
    Dictionary Should Contain Key    ${twin}    temp_u16
    Dictionary Should Contain Key    ${twin}    coil_rw
    Dictionary Should Not Contain Key    ${twin}    level_f32

Parameter Update Command Writes The Points And Completes
    [Documentation]    (flows) A Cumulocity-shaped parameter_update command (as the c8y mapper
    ...                would publish for a c8y_ParameterUpdate operation) is bridged to ONE
    ...                connector write-batch, completes with the mapper metadata preserved, and
    ...                the twin reflects the new values.
    [Tags]    flows
    Publish Message    ${PARAM_CMD_PREFIX}/c8y-mapper-1
    ...    {"status":"init","operation":{"deviceId":"1","c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${PROTOCOL}_parameters":{},"${PROTOCOL}_parameters":{"temp_u16":1234,"coil_rw":false}},"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}}    retain=True
    ${result}=    Wait For Message Containing    ${PARAM_CMD_PREFIX}/c8y-mapper-1    "status":"successful"    timeout=${FLOWS_TIMEOUT}
    ${meta}=    Get Json Field    ${result}    c8y-mapper.on_fragment
    Should Be Equal    ${meta}    c8y_ParameterUpdate
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    ${batch}=    Wait For Message Containing    ${BATCH_PREFIX}/ot--c8y-mapper-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${twin}=    Wait For Message Containing    ${PARAM_TWIN}    "temp_u16":1234    timeout=${FLOWS_TIMEOUT}
    ${coil}=    Get Json Field    ${twin}    coil_rw
    Should Be Equal    ${coil}    ${False}
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temp_u16    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    1234

Parameter Update With An Unknown Key Fails With The Connector Reason
    [Tags]    flows
    Publish Message    ${PARAM_CMD_PREFIX}/c8y-mapper-2
    ...    {"status":"init","operation":{"c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${PROTOCOL}_parameters":{},"${PROTOCOL}_parameters":{"bogus":1}},"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}}    retain=True
    ${result}=    Wait For Message Containing    ${PARAM_CMD_PREFIX}/c8y-mapper-2    "status":"failed"    timeout=${FLOWS_TIMEOUT}
    ${reason}=    Get Json Field    ${result}    reason
    Should Contain    ${reason}    bogus

Generic Write Command Is Bridged By The Flows
    [Documentation]    (flows) The pre-existing ot_write bridge (c8y_SetRegister path) still works
    ...                alongside the parameter bridge.
    [Tags]    flows
    Publish Message    te/device/${DEVICE}///cmd/ot_write/w-1    {"status":"init","point":"temp_u16","value":17001}    retain=True
    ${result}=    Wait For Message Containing    te/device/${DEVICE}///cmd/ot_write/w-1    "status":"successful"    timeout=${FLOWS_TIMEOUT}
    ${twin}=    Wait For Message Containing    ${PARAM_TWIN}    "temp_u16":17001    timeout=${FLOWS_TIMEOUT}


*** Keywords ***
Sample Should Be Good
    [Arguments]    ${payload}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    good
    ${mode}=    Get Json Field    ${payload}    mode
    Should Be Equal    ${mode}    typed
