package com.audiorelay.flutter.audio_relay_flutter.network

import org.json.JSONObject

/**
 * TCP control-channel messages. Mirrors
 * `desktop-app/src/protocol/control.rs` field-for-field — see
 * `/protocol-spec.md` §4 for the state machine this implements the client
 * side of.
 */
sealed class ControlMessage {

    data class Hello(
        val protocol_version: Int,
        val device_id: String,
        val device_name: String,
        val audio_port: Int,
    ) : ControlMessage()

    data class HelloAck(
        val protocol_version: Int,
        val device_id: String,
        val device_name: String,
        val paired: Boolean,
        val nonce: String,
    ) : ControlMessage()

    data class PairRequest(
        val proof: String,
    ) : ControlMessage()

    data class Repair(val device_id: String, val proof: String) : ControlMessage()

    data class PairOk(val session_id: String) : ControlMessage()

    data class PairFail(val reason: String) : ControlMessage()

    data class Capabilities(val sample_rate: Int, val channels: Int) : ControlMessage()

    data class Ping(val t: Long) : ControlMessage()

    data class Pong(val t: Long) : ControlMessage()

    object Bye : ControlMessage()

    /** Serializes to one newline-terminated JSON line, ready to write to the socket. */
    fun toLine(): String {
        val obj = JSONObject()
        when (this) {
            is Hello -> {
                obj.put("type", "HELLO")
                obj.put("protocol_version", protocol_version)
                obj.put("device_id", device_id)
                obj.put("device_name", device_name)
                obj.put("audio_port", audio_port)
            }
            is HelloAck -> {
                obj.put("type", "HELLO_ACK")
                obj.put("protocol_version", protocol_version)
                obj.put("device_id", device_id)
                obj.put("device_name", device_name)
                obj.put("paired", paired)
                obj.put("nonce", nonce)
            }
            is PairRequest -> {
                obj.put("type", "PAIR_REQUEST")
                obj.put("proof", proof)
            }
            is Repair -> {
                obj.put("type", "REPAIR")
                obj.put("device_id", device_id)
                obj.put("proof", proof)
            }
            is PairOk -> {
                obj.put("type", "PAIR_OK")
                obj.put("session_id", session_id)
            }
            is PairFail -> {
                obj.put("type", "PAIR_FAIL")
                obj.put("reason", reason)
            }
            is Capabilities -> {
                obj.put("type", "CAPABILITIES")
                obj.put("sample_rate", sample_rate)
                obj.put("channels", channels)
            }
            is Ping -> {
                obj.put("type", "PING")
                obj.put("t", t)
            }
            is Pong -> {
                obj.put("type", "PONG")
                obj.put("t", t)
            }
            is Bye -> {
                obj.put("type", "BYE")
            }
        }
        return obj.toString() + "\n"
    }

    companion object {
        fun parseLine(line: String): ControlMessage? {
            val trimmed = line.trim()
            if (trimmed.isEmpty()) return null
            return try {
                val obj = JSONObject(trimmed)
                when (obj.optString("type")) {
                    "HELLO" -> Hello(
                        obj.getInt("protocol_version"),
                        obj.getString("device_id"),
                        obj.getString("device_name"),
                        obj.getInt("audio_port")
                    )
                    "HELLO_ACK" -> HelloAck(
                        obj.getInt("protocol_version"),
                        obj.getString("device_id"),
                        obj.getString("device_name"),
                        obj.getBoolean("paired"),
                        obj.getString("nonce")
                    )
                    "PAIR_REQUEST" -> PairRequest(obj.getString("proof"))
                    "REPAIR" -> Repair(obj.getString("device_id"), obj.getString("proof"))
                    "PAIR_OK" -> PairOk(obj.getString("session_id"))
                    "PAIR_FAIL" -> PairFail(obj.getString("reason"))
                    "CAPABILITIES" -> Capabilities(obj.getInt("sample_rate"), obj.getInt("channels"))
                    "PING" -> Ping(obj.getLong("t"))
                    "PONG" -> Pong(obj.getLong("t"))
                    "BYE" -> Bye
                    else -> null
                }
            } catch (e: Exception) {
                null
            }
        }
    }
}
