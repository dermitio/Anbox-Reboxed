package org.anbox.reboxed.bridge

import org.json.JSONObject
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.EOFException

object BridgeProtocol {
    const val VERSION = 1
    private const val MAX_FRAME = 1024 * 1024

    fun read(input: DataInputStream): JSONObject {
        val size = try { input.readInt() } catch (error: EOFException) { throw error }
        require(size in 1..MAX_FRAME) { "Invalid bridge frame length" }
        val data = ByteArray(size)
        input.readFully(data)
        val value = JSONObject(String(data, Charsets.UTF_8))
        require(value.optInt("version", -1) == VERSION) { "Unsupported bridge protocol" }
        return value
    }

    @Synchronized
    fun write(output: DataOutputStream, value: JSONObject) {
        value.put("version", VERSION)
        val data = value.toString().toByteArray(Charsets.UTF_8)
        require(data.size in 1..MAX_FRAME) { "Bridge frame is oversized" }
        output.writeInt(data.size)
        output.write(data)
        output.flush()
    }
}
