package com.aiwatch.companion.data

import android.content.Context
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.dataStore by preferencesDataStore(name = "settings")

/** 应用设置（DataStore） */
class SettingsRepository(private val context: Context) {

    private val keyKeepAlive = booleanPreferencesKey("keep_alive_enabled")

    /** 前台服务保活连接（默认开启） */
    val keepAlive: Flow<Boolean> = context.dataStore.data.map { it[keyKeepAlive] ?: true }

    suspend fun setKeepAlive(enabled: Boolean) {
        context.dataStore.edit { it[keyKeepAlive] = enabled }
    }
}
