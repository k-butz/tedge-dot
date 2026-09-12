*** Settings ***
Documentation       Device lifecycle for the Cumulocity (cloud) e2e suites: DeviceLibrary starts
...                 the protocol's docker compose stack as an isolated project named after a
...                 randomly generated device serial, the image's bootstrap script registers that
...                 serial with the tenant, and the teardown deletes the device and its user
...                 again. Nothing has to be started before the tests, and no manual tenant
...                 cleanup is needed.
...
...                 Per suite (one device for the whole file, which the ordered cloud suites
...                 rely on):
...                 | Suite Setup       Setup Cloud Device
...                 | Suite Teardown    Teardown Cloud Device
...
...                 Per test (a fresh device per test case) works the same way from
...                 `Test Setup` / `Test Teardown`; the containers are reclaimed at suite end.
...
...                 Needs C8Y_BASEURL / C8Y_USER / C8Y_PASSWORD / C8Y_TENANT in the environment
...                 or the repo's .env file — the compose file forwards them to the container,
...                 and DeviceLibrary passes the generated DEVICE_ID alongside.
...
...                 IMPL (rust by default, or c) selects which connector implementation the
...                 image installs, so the same suites cover both — `just test-cloud <proto>`
...                 and `just test-cloud-c <proto>`.

Library             Cumulocity
Library             DeviceLibrary
Library             OperatingSystem


*** Variables ***
&{C8Y_CONFIG}               host=%{C8Y_BASEURL= }    username=%{C8Y_USER= }    password=%{C8Y_PASSWORD= }    tenant=%{C8Y_TENANT= }
# Seconds allowed for `docker compose up --wait` (image builds are not counted against it).
${STACK_WAIT_TIMEOUT}       300
# Seconds to wait for the device to appear in the tenant after bootstrapping.
${REGISTRATION_TIMEOUT}     60
# Set to a truthy value (robot --variable KEEP_STACK:true) to leave the containers running after
# the suite. The tenant-side device is still deleted by the teardown.
${KEEP_STACK}               ${EMPTY}


*** Keywords ***
Setup Cloud Device
    [Documentation]    Start the cloud stack, bootstrap the device and make it the current
    ...    Cumulocity device. Sets the ${DEVICE_ID} suite variable (the generated serial, which
    ...    is also the device's external id).
    [Arguments]    ${protocol}=modbus

    ${cleanup}=    Evaluate    not $KEEP_STACK
    ${device_id}=    DeviceLibrary.Setup
    ...    compose_file=${CURDIR}/../${protocol}/docker-compose.yaml
    ...    skip_bootstrap=${True}
    ...    build=${True}
    ...    wait_timeout=${STACK_WAIT_TIMEOUT}
    ...    cleanup=${cleanup}
    Set Suite Variable    $DEVICE_ID    ${device_id}
    ${project}=    DeviceLibrary.Get Compose Project Name
    Log    Started ${protocol} cloud stack as compose project '${project}' (device ${device_id})
    # Which connector the image installed (see cloud/<proto>/Dockerfile.tedge): IMPL=rust
    # installs the packaged .deb, IMPL=c the C build (just test-cloud-c).
    ${impl}=    Get Environment Variable    IMPL    rust
    ${container}=    DeviceLibrary.Get Container Name
    Log    Connector under test: ${impl} implementation (container ${container})
    Connector Implementation Should Be    ${impl}

    Bootstrap Device
    Cumulocity.Device Should Exist    ${DEVICE_ID}    timeout=${REGISTRATION_TIMEOUT}

Connector Implementation Should Be
    [Documentation]    Guard against a silent fallback: the compose file defaults IMPL to rust,
    ...    so a mistyped or unexported variable would quietly test the wrong binary and report a
    ...    green C run. The Rust connector is installed as the tedge-dot-rs package; the C one is copied into
    ...    the image by its build stage, with no package behind it.
    [Arguments]    ${impl}
    ${packaged}=    DeviceLibrary.Execute Command
    ...    cmd=dpkg -s tedge-dot-rs >/dev/null 2>&1 && echo packaged || echo unpackaged
    ...    strip=${True}
    IF    "${impl}" == "rust"
        Should Be Equal    ${packaged}    packaged
        ...    msg=IMPL=rust but no tedge-dot-rs package is installed in the image
    ELSE
        Should Be Equal    ${packaged}    unpackaged
        ...    msg=IMPL=${impl} but the Rust tedge-dot-rs package is installed in the image
    END

Bootstrap Device
    [Documentation]    Register the device with Cumulocity using the demo image's bootstrap
    ...    script. It reads DEVICE_ID and the C8Y_* credentials from the container environment
    ...    (injected by the compose file), so no secrets appear in the Robot log.
    DeviceLibrary.Execute Command    cmd=bootstrap.sh

Collect Device Logs
    Run Keyword And Continue On Failure    DeviceLibrary.Execute Command
    ...    cmd=journalctl -n 10000 -u "tedge-*" -u "c8y-*" --no-pager    ignore_exit_code=${True}
    Run Keyword And Continue On Failure    DeviceLibrary.Get Service Logs

Teardown Cloud Device
    [Documentation]    Collect the device logs, then remove the device and its user from the
    ...    tenant. DeviceLibrary stops and removes the compose project at suite end.
    Run Keyword And Ignore Error    Collect Device Logs
    Run Keyword And Ignore Error    Cumulocity.Delete Managed Object And Device User
    ...    external_id=${DEVICE_ID}
