//! Device *parameters*: which points a configuration exposes as operator-editable settings,
//! and how to declare them in Cumulocity's Digital Twin Manager (DTM).
//!
//! A parameter is a point whose `access` permits writes, plus any point that opts in through
//! `meta.parameter` (`meta.parameter = false` opts a writable point out). Parameters are grouped
//! into **sets** (`meta.parameter.set`, default `<protocol>_parameters`): one set is one twin
//! fragment on the device (published by the `ot-parameter-state` flow with the current values)
//! and one DTM property definition in the tenant (rendered by `tedge-dot describe`). The keys of
//! a set are the point ids, so parameter ids must be plain identifiers (`[A-Za-z0-9_]`).
//!
//! `meta.parameter` (all optional; either a string naming the set, `true`, or a table):
//!
//! ```toml
//! [[device.point]]
//! id       = "setpoint"
//! datatype = "int16"
//! access   = "read_write"
//! unit     = "°C"
//! meta.parameter = { set = "boiler", title = "Setpoint", min = 0, max = 120, order = 1 }
//! ```

use crate::config::{ConnectorConfig, PointConfig};
use crate::connector::Access;
use crate::model::DataType;
use serde_json::{json, Map, Value};

/// Default parameter set name for a protocol: `<protocol>_parameters`.
pub fn default_set(protocol: &str) -> String {
    format!("{}_parameters", protocol.replace(|c: char| !c.is_ascii_alphanumeric(), "_"))
}

/// True when `id` can be used verbatim as a fragment key (Cumulocity rejects `.` and `$`).
pub fn is_valid_key(id: &str) -> bool {
    !id.is_empty() && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
}

/// One parameter derived from a configured point.
#[derive(Clone, Debug, PartialEq)]
pub struct Parameter {
    /// Point id (= the key inside the set).
    pub point: String,
    /// Parameter set (twin fragment / DTM identifier).
    pub set: String,
    pub datatype: Option<DataType>,
    pub access: Access,
    pub unit: Option<String>,
    /// The `meta.parameter` table (normalized to an object).
    pub options: Map<String, Value>,
}

/// The parameters of one point, if it is one. `default_set` names the set for points that do
/// not pick their own.
pub fn parameter_of(point: &PointConfig, default_set: &str) -> Option<Parameter> {
    let access = Access::parse(point.access.as_deref());
    let options: Option<Map<String, Value>> = match point.meta.as_ref().and_then(|m| m.get("parameter")) {
        None => None,
        Some(Value::Bool(false)) => return None, // explicit opt-out
        Some(Value::Bool(true)) => Some(Map::new()),
        Some(Value::String(set)) => {
            let mut m = Map::new();
            m.insert("set".into(), Value::String(set.clone()));
            Some(m)
        }
        Some(Value::Object(m)) => Some(m.clone()),
        Some(_) => Some(Map::new()),
    };
    if !access.can_write() && options.is_none() {
        return None;
    }
    let options = options.unwrap_or_default();
    let set = options
        .get("set")
        .and_then(|s| s.as_str())
        .map(String::from)
        .unwrap_or_else(|| default_set.to_string());
    Some(Parameter {
        point: point.id.clone(),
        set,
        datatype: point.datatype,
        access,
        unit: point.unit.clone(),
        options,
    })
}

/// Every parameter of every device in the config, in configuration order.
pub fn parameters(config: &ConnectorConfig, default_set: &str) -> Vec<Parameter> {
    config
        .devices
        .iter()
        .flat_map(|d| d.points.iter().filter_map(|p| parameter_of(p, default_set)))
        .collect()
}

/// Parameter ids (and set names) that cannot be used as fragment keys.
pub fn invalid_keys(config: &ConnectorConfig, default_set: &str) -> Vec<String> {
    parameters(config, default_set)
        .into_iter()
        .flat_map(|p| {
            let mut bad = Vec::new();
            if !is_valid_key(&p.point) {
                bad.push(format!("point id '{}'", p.point));
            }
            if !is_valid_key(&p.set) {
                bad.push(format!("parameter set '{}'", p.set));
            }
            bad
        })
        .collect()
}

/// Render Cumulocity Digital Twin Manager property definitions — one per parameter set — for
/// every device in the config. The output is the request body of
/// `POST /service/dtm/definitions/properties` (one element at a time), which a tenant admin
/// registers once; the device never talks to the DTM service. Sets that appear on several
/// devices are merged (the identifier is tenant-wide, so devices sharing a set must agree).
pub fn c8y_dtm_definitions(config: &ConnectorConfig, default_set: Option<&str>) -> Vec<Value> {
    let default_set = default_set
        .map(String::from)
        .unwrap_or_else(|| self::default_set(&config.connector.protocol));
    // set -> ordered (key, property schema)
    let mut sets: Vec<(String, Vec<(String, Value)>)> = Vec::new();
    for param in parameters(config, &default_set) {
        let entry = match sets.iter_mut().find(|(name, _)| *name == param.set) {
            Some(e) => e,
            None => {
                sets.push((param.set.clone(), Vec::new()));
                sets.last_mut().unwrap()
            }
        };
        if entry.1.iter().any(|(k, _)| *k == param.point) {
            continue; // same key on another device: first definition wins
        }
        let schema = property_schema(&param);
        entry.1.push((param.point.clone(), schema));
    }
    sets.into_iter()
        .map(|(set, props)| {
            let mut properties = Map::new();
            for (i, (key, mut schema)) in props.into_iter().enumerate() {
                if !schema.as_object().map(|o| o.contains_key("order")).unwrap_or(false) {
                    schema["order"] = json!(i + 1);
                }
                properties.insert(key, schema);
            }
            json!({
                "identifier": set,
                "jsonSchema": {
                    "$schema": "http://json-schema.org/draft-07/schema#",
                    "title": title_from_key(&set),
                    "description": format!(
                        "Writable {} points exposed by tedge-dot (generated from the connector configuration)",
                        config.connector.protocol
                    ),
                    "type": "object",
                    "properties": properties,
                },
                "contexts": ["asset", "operation"],
                "tags": ["tedge-dot", config.connector.protocol],
            })
        })
        .collect()
}

/// JSON-schema property for one parameter (type from the datatype, limits from the datatype
/// range, everything else from `meta.parameter`).
pub fn property_schema(param: &Parameter) -> Value {
    let mut schema = Map::new();
    let (ty, min, max): (&str, Option<f64>, Option<f64>) = match param.datatype {
        Some(DataType::Bool) => ("boolean", None, None),
        Some(DataType::Int8) => ("integer", Some(i8::MIN as f64), Some(i8::MAX as f64)),
        Some(DataType::Uint8) => ("integer", Some(0.0), Some(u8::MAX as f64)),
        Some(DataType::Int16) => ("integer", Some(i16::MIN as f64), Some(i16::MAX as f64)),
        Some(DataType::Uint16) => ("integer", Some(0.0), Some(u16::MAX as f64)),
        Some(DataType::Int32) => ("integer", Some(i32::MIN as f64), Some(i32::MAX as f64)),
        Some(DataType::Uint32) => ("integer", Some(0.0), Some(u32::MAX as f64)),
        // 64-bit limits exceed the JS safe range; leave them unbounded.
        Some(DataType::Int64) | Some(DataType::Uint64) => ("integer", None, None),
        Some(DataType::Float32) | Some(DataType::Float64) => ("number", None, None),
        Some(DataType::String) | Some(DataType::Bytes) | None => ("string", None, None),
    };
    schema.insert("type".into(), json!(ty));
    let title = param
        .options
        .get("title")
        .and_then(|t| t.as_str())
        .map(String::from)
        .unwrap_or_else(|| param.point.clone());
    schema.insert("title".into(), json!(title));
    let mut description = param
        .options
        .get("description")
        .and_then(|d| d.as_str())
        .map(String::from)
        .unwrap_or_default();
    if let Some(unit) = &param.unit {
        if !description.is_empty() {
            description.push(' ');
        }
        description.push_str(&format!("[{unit}]"));
    }
    if param.access == Access::Write {
        if !description.is_empty() {
            description.push(' ');
        }
        description.push_str("(write-only: shows the last value written)");
    }
    if !description.is_empty() {
        schema.insert("description".into(), json!(description));
    }
    let min = param.options.get("min").and_then(|v| v.as_f64()).or(min);
    let max = param.options.get("max").and_then(|v| v.as_f64()).or(max);
    if let Some(min) = min {
        schema.insert("minimum".into(), json!(min));
    }
    if let Some(max) = max {
        schema.insert("maximum".into(), json!(max));
    }
    for key in ["enum", "default", "order"] {
        if let Some(v) = param.options.get(key) {
            schema.insert(key.into(), v.clone());
        }
    }
    if !param.access.can_write() {
        schema.insert("readOnly".into(), json!(true));
    }
    Value::Object(schema)
}

fn title_from_key(key: &str) -> String {
    let mut out = String::new();
    for (i, part) in key.split('_').filter(|p| !p.is_empty()).enumerate() {
        if i > 0 {
            out.push(' ');
        }
        let mut chars = part.chars();
        if let Some(first) = chars.next() {
            if i == 0 {
                out.extend(first.to_uppercase());
            } else {
                out.push(first);
            }
            out.push_str(chars.as_str());
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const CONFIG: &str = r#"
[connector]
protocol = "modbus"

[[device]]
name = "plc1"
protocol_address = { transport = "tcp", host = "127.0.0.1", port = 502, unit_id = 1 }

  [[device.point]]
  id = "temp_u16"
  datatype = "uint16"
  access = "read_write"
  unit = "°C"
  address = { table = "holding", address = 3, count = 1 }
  meta = { parameter = { title = "Temperature setpoint", min = 0, max = 100, order = 7 } }

  [[device.point]]
  id = "coil_rw"
  datatype = "bool"
  access = "read_write"
  address = { table = "coil", address = 48, count = 1 }

  [[device.point]]
  id = "pump_speed"
  datatype = "float32"
  access = "write"
  address = { table = "holding", address = 10, count = 2 }
  meta = { parameter = "pump" }

  [[device.point]]
  id = "level_f32"
  datatype = "float32"
  address = { table = "holding", address = 6, count = 2 }

  [[device.point]]
  id = "status_word"
  datatype = "uint16"
  address = { table = "holding", address = 20, count = 1 }
  meta = { parameter = true }

  [[device.point]]
  id = "hidden_rw"
  datatype = "uint16"
  access = "read_write"
  address = { table = "holding", address = 21, count = 1 }
  meta = { parameter = false }
"#;

    fn cfg() -> ConnectorConfig {
        toml::from_str(CONFIG).unwrap()
    }

    #[test]
    fn parameters_select_writable_and_opted_in_points() {
        let params = parameters(&cfg(), "modbus_parameters");
        let names: Vec<(&str, &str)> = params
            .iter()
            .map(|p| (p.point.as_str(), p.set.as_str()))
            .collect();
        assert_eq!(
            names,
            vec![
                ("temp_u16", "modbus_parameters"),
                ("coil_rw", "modbus_parameters"),
                ("pump_speed", "pump"),
                ("status_word", "modbus_parameters"),
            ]
        );
        assert_eq!(params[3].access, Access::Read);
    }

    #[test]
    fn key_validation_and_default_set() {
        assert!(is_valid_key("ok_id_1"));
        assert!(!is_valid_key("Environment.Temperature"));
        assert!(!is_valid_key(""));
        assert_eq!(default_set("opc-ua"), "opc_ua_parameters");
        let mut c = cfg();
        c.devices[0].points[0].id = "Boiler.Temp".into();
        let bad = invalid_keys(&c, "modbus_parameters");
        assert_eq!(bad, vec!["point id 'Boiler.Temp'"]);
        assert!(invalid_keys(&cfg(), "plant.floor").iter().all(|b| b.contains("parameter set")));
    }

    #[test]
    fn dtm_definitions_group_by_set_and_render_schema() {
        let defs = c8y_dtm_definitions(&cfg(), None);
        assert_eq!(defs.len(), 2);
        let main = &defs[0];
        assert_eq!(main["identifier"], "modbus_parameters");
        assert_eq!(main["contexts"], json!(["asset", "operation"]));
        let props = &main["jsonSchema"]["properties"];
        assert_eq!(props["temp_u16"]["type"], "integer");
        assert_eq!(props["temp_u16"]["title"], "Temperature setpoint");
        assert_eq!(props["temp_u16"]["minimum"], 0.0);
        assert_eq!(props["temp_u16"]["maximum"], 100.0);
        assert_eq!(props["temp_u16"]["order"], 7);
        assert_eq!(props["temp_u16"]["description"], "[°C]");
        assert_eq!(props["coil_rw"]["type"], "boolean");
        assert_eq!(props["coil_rw"]["order"], 2);
        assert_eq!(props["status_word"]["readOnly"], true);
        assert_eq!(props["status_word"]["maximum"], 65535.0);
        assert!(props.get("hidden_rw").is_none());
        assert!(props.get("level_f32").is_none());

        let pump = &defs[1];
        assert_eq!(pump["identifier"], "pump");
        assert_eq!(pump["jsonSchema"]["title"], "Pump");
        let speed = &pump["jsonSchema"]["properties"]["pump_speed"];
        assert_eq!(speed["type"], "number");
        assert!(speed["description"].as_str().unwrap().contains("write-only"));
    }

    #[test]
    fn dtm_default_set_override() {
        let defs = c8y_dtm_definitions(&cfg(), Some("plant_settings"));
        assert_eq!(defs[0]["identifier"], "plant_settings");
    }
}
