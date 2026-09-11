*** Settings ***
Documentation       End-to-end tests for the Rust tedge-dot (opcua module) against a real
...                 OPC-UA server (python-asyncua). The connector reads the simulator's nodes and
...                 publishes samples + status to a local MQTT broker; these tests assert on that
...                 output. No cloud (Cumulocity) is involved. This proves the connector contract
...                 and SDK runtime are protocol-neutral: the same envelopes a Modbus driver emits
...                 are produced here by an OPC-UA driver with NodeId addressing.
...
...                 Run via:  just test-e2e-opcua   (brings the Docker stack up/down automatically)

Library             ../../_shared/MqttClient.py
Library             Collections

Suite Setup         Connect And Subscribe
Suite Teardown      Disconnect Broker


*** Variables ***
${BROKER_HOST}          localhost
${BROKER_PORT}          12883

${DEVICE}               opc1
${PROTOCOL}             opcua
${SERVICE}              tedge-dot

${SAMPLE_PREFIX}        te/device/${DEVICE}/ot/${PROTOCOL}/sample
${CMD_PREFIX}           te/device/${DEVICE}/ot/${PROTOCOL}/cmd/write
${LINK_TOPIC}           te/device/${DEVICE}/ot/${PROTOCOL}/status/link
${CAPS_TOPIC}           te/device/main/service/${SERVICE}/ot/capabilities
${HEALTH_TOPIC}         te/device/main/service/${SERVICE}/status/health
${BATCH_PREFIX}         te/device/${DEVICE}/ot/${PROTOCOL}/cmd/write-batch
${PARAM_CMD_PREFIX}     te/device/${DEVICE}///cmd/ot_parameter_update
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
    Should Be Equal    ${protocol}    opcua
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write

Service Health Is Up
    [Documentation]    The connector publishes a retained service health status of "up".
    ${payload}=    Wait For Retained    ${HEALTH_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    up

Device Link Is Connected
    [Documentation]    The connector reports the OPC-UA server link as connected.
    ${payload}=    Wait For Retained    ${LINK_TOPIC}    timeout=${READY_TIMEOUT}
    ${status}=    Get Json Field    ${payload}    status
    Should Be Equal    ${status}    connected

Reads Float64 Node
    [Documentation]    Reads the Temperature node (Double 21.5) addressed by NodeId ns=2;s=Temperature.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temperature    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${datatype}=    Get Json Field    ${payload}    datatype
    Should Be Equal    ${datatype}    float64
    ${value}=    Get Json Field    ${payload}    value
    Should Be True    abs(${value} - 21.5) < 0.05

Sample Echoes The Node Id
    [Documentation]    The sample's addr field echoes the OPC-UA NodeId it was read from.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temperature    timeout=${SAMPLE_TIMEOUT}
    ${node}=    Get Json Field    ${payload}    addr.node_id
    Should Contain    ${node}    Temperature

Reads Uint32 Node
    [Documentation]    Reads the Count node (UInt32 617001).
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/count_u32    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${payload}
    ${datatype}=    Get Json Field    ${payload}    datatype
    Should Be Equal    ${datatype}    uint32
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    617001

Unknown Node Reports Bad Quality
    [Documentation]    Reading a non-existent NodeId yields a bad-quality sample with an error.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/bad_point    timeout=${SAMPLE_TIMEOUT}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    bad
    ${error}=    Get Json Field    ${payload}    error
    Should Not Be Empty    ${error}

Writes An Int32 Node And Reads It Back
    [Documentation]    A write command sets Setpoint to 4242; the next sample reflects it.
    Publish Message    ${CMD_PREFIX}/sp-1    {"status":"init","point":"setpoint","value":4242}    retain=True
    ${result}=    Wait For Message Containing    ${CMD_PREFIX}/sp-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${point}=    Get Json Field    ${result}    point
    Should Be Equal    ${point}    setpoint
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/setpoint    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    4242

Writes A Boolean Node And Reads It Back
    [Documentation]    A write command sets Running true; the next sample reflects it.
    Publish Message    ${CMD_PREFIX}/run-1    {"status":"init","point":"running","value":true}    retain=True
    ${result}=    Wait For Message Containing    ${CMD_PREFIX}/run-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${point}=    Get Json Field    ${result}    point
    Should Be Equal    ${point}    running
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/running    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal    ${value}    ${True}

Subscribed Node Pushes Value Changes
    [Documentation]    The ticks point is delivered by an OPC-UA subscription (monitored item),
    ...                not polling: the simulator increments it every second and each change
    ...                arrives as a pushed sample with a strictly increasing value.
    ${first}=    Wait For Sample    ${SAMPLE_PREFIX}/ticks    timeout=${SAMPLE_TIMEOUT}
    Sample Should Be Good    ${first}
    ${v1}=    Get Json Field    ${first}    value
    ${second}=    Wait For Sample    ${SAMPLE_PREFIX}/ticks    timeout=${SAMPLE_TIMEOUT}
    ${v2}=    Get Json Field    ${second}    value
    Should Be True    ${v2} > ${v1}

Pushed Sample Echoes Point Meta
    [Documentation]    The point's free-form meta table (connector config) is echoed verbatim
    ...                in the sample envelope, so flows can apply per-signal behaviour.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/ticks    timeout=${SAMPLE_TIMEOUT}
    ${on_change}=    Get Json Field    ${payload}    meta.on_change
    Should Be Equal    ${on_change}    ${True}
    ${source}=    Get Json Field    ${payload}    meta.source
    Should Be Equal    ${source}    sim

Polled Sample Carries The Device Name
    [Documentation]    Regression: the runtime stamps the configured device name on polled
    ...                samples (topic AND envelope), even when the driver leaves it empty.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temperature    timeout=${SAMPLE_TIMEOUT}
    ${device}=    Get Json Field    ${payload}    device
    Should Be Equal    ${device}    ${DEVICE}


Samples Carry The Point Access
    [Documentation]    Every sample echoes the point's declared access, so flows can tell
    ...                writable points (parameters) apart without reading the config file.
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/setpoint    timeout=${SAMPLE_TIMEOUT}
    ${access}=    Get Json Field    ${payload}    access
    Should Be Equal    ${access}    read_write
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/temperature    timeout=${SAMPLE_TIMEOUT}
    ${access}=    Get Json Field    ${payload}    access
    Should Be Equal    ${access}    read

Capability Descriptor Advertises Write Batch
    [Documentation]    The runtime adds the write-batch verb for every module that implements write.
    ${payload}=    Wait For Retained    ${CAPS_TOPIC}    timeout=${READY_TIMEOUT}
    ${verbs}=    Get Json Field    ${payload}    command_verbs
    List Should Contain Value    ${verbs}    write-batch

Write Batch Writes Several Points In One Command
    [Documentation]    One write-batch request writes an int32 and a boolean node in order and reports
    ...                a per-point result; the next samples reflect both values.
    Publish Message    ${BATCH_PREFIX}/batch-1
    ...    {"status":"init","writes":[{"point":"setpoint","value":4243},{"point":"running","value":true}]}    retain=True
    ${result}=    Wait For Message Containing    ${BATCH_PREFIX}/batch-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    Should Be Equal    ${results}[0][point]    setpoint
    Should Be Equal    ${results}[0][status]    successful
    Should Be Equal    ${results}[1][point]    running
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/setpoint    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal As Numbers    ${value}    4243
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/running    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal    ${value}    ${True}

Write Batch Stops At The First Failure And Reports What Was Applied
    [Documentation]    A batch with an unknown point fails, but the result lists the write that
    ...                succeeded before it so the requester knows the device state.
    Publish Message    ${BATCH_PREFIX}/batch-2
    ...    {"status":"init","writes":[{"point":"setpoint","value":17001},{"point":"no_such_point","value":1},{"point":"running","value":false}]}    retain=True
    ${result}=    Wait For Message Containing    ${BATCH_PREFIX}/batch-2    "status":"failed"    timeout=${SAMPLE_TIMEOUT}
    ${reason}=    Get Json Field    ${result}    reason
    Should Contain    ${reason}    no_such_point
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    Should Be Equal    ${results}[0][status]    successful
    Should Be Equal    ${results}[1][status]    failed
    # the coil after the failing entry was never written
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/running    timeout=${SAMPLE_TIMEOUT}
    ${value}=    Get Json Field    ${payload}    value
    Should Be Equal    ${value}    ${True}

Write Batch Rejects An Empty Request
    Publish Message    ${BATCH_PREFIX}/batch-3    {"status":"init","writes":[]}    retain=True
    ${result}=    Wait For Message Containing    ${BATCH_PREFIX}/batch-3    "status":"failed"    timeout=${SAMPLE_TIMEOUT}
    ${reason}=    Get Json Field    ${result}    reason
    Should Contain    ${reason}    no writes

Flows Register The Device And Advertise The Parameter Capability
    [Documentation]    (flows) ot-registration turns the link status into a child-device
    ...                registration and advertises ot_parameter_update so a cloud mapper routes
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
    ${payload}=    Wait For Message Containing    ${PARAM_TWIN}    "setpoint":    timeout=${FLOWS_TIMEOUT}
    ${twin}=    Evaluate    json.loads($payload)    modules=json
    Dictionary Should Contain Key    ${twin}    setpoint
    Dictionary Should Contain Key    ${twin}    running
    Dictionary Should Not Contain Key    ${twin}    temperature

Parameter Update Command Writes The Points And Completes
    [Documentation]    (flows) A Cumulocity-shaped ot_parameter_update command (as the c8y mapper
    ...                would publish for a c8y_ParameterUpdate operation) is bridged to ONE
    ...                connector write-batch, completes with the mapper metadata preserved, and
    ...                the twin reflects the new values.
    [Tags]    flows
    Publish Message    ${PARAM_CMD_PREFIX}/c8y-mapper-1
    ...    {"status":"init","operation":{"deviceId":"1","c8y_ParameterUpdate":{},"c8y_ParameterUpdate_${PROTOCOL}_parameters":{},"${PROTOCOL}_parameters":{"setpoint":1234,"running":false}},"c8y-mapper":{"on_fragment":"c8y_ParameterUpdate","output":null}}    retain=True
    ${result}=    Wait For Message Containing    ${PARAM_CMD_PREFIX}/c8y-mapper-1    "status":"successful"    timeout=${FLOWS_TIMEOUT}
    ${meta}=    Get Json Field    ${result}    c8y-mapper.on_fragment
    Should Be Equal    ${meta}    c8y_ParameterUpdate
    ${results}=    Get Json Field    ${result}    results
    Length Should Be    ${results}    2
    ${batch}=    Wait For Message Containing    ${BATCH_PREFIX}/ot--c8y-mapper-1    "status":"successful"    timeout=${SAMPLE_TIMEOUT}
    ${twin}=    Wait For Message Containing    ${PARAM_TWIN}    "setpoint":1234    timeout=${FLOWS_TIMEOUT}
    ${coil}=    Get Json Field    ${twin}    running
    Should Be Equal    ${coil}    ${False}
    ${payload}=    Wait For Sample    ${SAMPLE_PREFIX}/setpoint    timeout=${SAMPLE_TIMEOUT}
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
    Publish Message    te/device/${DEVICE}///cmd/ot_write/w-1    {"status":"init","point":"setpoint","value":17001}    retain=True
    ${result}=    Wait For Message Containing    te/device/${DEVICE}///cmd/ot_write/w-1    "status":"successful"    timeout=${FLOWS_TIMEOUT}
    ${twin}=    Wait For Message Containing    ${PARAM_TWIN}    "setpoint":17001    timeout=${FLOWS_TIMEOUT}


*** Keywords ***
Connect And Subscribe
    Connect Broker    ${BROKER_HOST}    ${BROKER_PORT}
    Subscribe    te/#

Sample Should Be Good
    [Arguments]    ${payload}
    ${quality}=    Get Json Field    ${payload}    quality
    Should Be Equal    ${quality}    good
    ${mode}=    Get Json Field    ${payload}    mode
    Should Be Equal    ${mode}    typed
