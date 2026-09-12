*** Settings ***
Documentation       Stack lifecycle for the connector e2e suites: DeviceLibrary starts the
...                 protocol's docker compose file as an isolated project (named after a
...                 randomly generated device serial) and tears it down when the suite ends,
...                 so no stack has to be started before the tests run.
...
...                 Use it per suite (one stack for the whole file):
...                 | Suite Setup       Setup OT Stack    modbus
...                 | Suite Teardown    Teardown OT Stack
...
...                 or per test (a fresh stack for every test case — slower, but fully
...                 isolated; the containers of each test are reclaimed when the suite ends):
...                 | Test Setup        Setup OT Stack    modbus
...                 | Test Teardown     Teardown OT Stack

Library             DeviceLibrary
Library             MqttClient.py
Library             OperatingSystem


*** Variables ***
# The broker publishes its MQTT port without a fixed host port, so every stack gets an
# ephemeral one and parallel runs cannot clash; `Setup OT Stack` resolves it. The host-networked
# stacks (canbus, canopen) instead pass `broker_port` explicitly — see the keyword.
${BROKER_CONTAINER_PORT}        1883
# Seconds allowed for `docker compose up --wait` (image builds are not counted against it).
${STACK_WAIT_TIMEOUT}           240
# Set to a truthy value to leave the stack running after the suite, to inspect it by hand:
#   robot --variable KEEP_STACK:true --test "..." connectors/modbus/tests/
# The project name is logged by the setup (docker compose -p <project> logs -f).
${KEEP_STACK}                   ${EMPTY}


*** Keywords ***
Setup OT Stack
    [Documentation]    Start the protocol's compose stack and subscribe to the broker.
    ...    `broker_port` pins the host port for stacks whose broker runs in the host network
    ...    namespace (canbus, canopen); leave it empty to resolve the published ephemeral port.
    [Arguments]    ${protocol}    ${broker_port}=${EMPTY}    ${device_service}=${EMPTY}

    ${cleanup}=    Evaluate    not $KEEP_STACK
    ${serial}=    DeviceLibrary.Setup
    ...    compose_file=${CURDIR}/../${protocol}/docker-compose.yaml
    ...    device_service=${device_service}
    ...    skip_bootstrap=${True}
    ...    build=${True}
    ...    wait_timeout=${STACK_WAIT_TIMEOUT}
    ...    cleanup=${cleanup}
    Set Suite Variable    $STACK_SERIAL    ${serial}
    ${project}=    DeviceLibrary.Get Compose Project Name
    Log    Started ${protocol} stack as compose project '${project}' (device ${serial})
    # Which implementation the stack built (see robot.toml profiles / just test-e2e-c):
    # IMPL selects the image name, CONNECTOR_DOCKERFILE the build recipe.
    ${impl}=    Get Environment Variable    IMPL    rust
    ${container}=    DeviceLibrary.Get Container Name
    Log    Connector under test: ${impl} implementation (container ${container})

    IF    "${broker_port}" == "${EMPTY}"
        ${host}    ${port}=    DeviceLibrary.Get Service Port
        ...    service=broker    port=${BROKER_CONTAINER_PORT}
    ELSE
        ${host}    ${port}=    Set Variable    localhost    ${broker_port}
    END
    Set Suite Variable    $BROKER_HOST    ${host}
    Set Suite Variable    $BROKER_PORT    ${port}
    Log    Broker reachable on ${host}:${port}

    Connect Broker    ${host}    ${port}
    Subscribe    te/#

Teardown OT Stack
    [Documentation]    Disconnect from the broker and, when the test failed, dump the stack's
    ...    container logs into the Robot log. DeviceLibrary stops and removes the compose
    ...    project (containers, network, volumes) itself when the suite ends.
    Run Keyword And Ignore Error    Collect Stack Logs
    Run Keyword And Ignore Error    Disconnect Broker

Collect Stack Logs
    [Documentation]    Container logs of every service of the stack (docker compose logs).
    DeviceLibrary.Get Service Logs
