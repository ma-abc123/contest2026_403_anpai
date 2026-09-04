package com.aiwatch.companion.ui

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Bluetooth
import androidx.compose.material.icons.outlined.Mic
import androidx.compose.material.icons.outlined.Notifications
import androidx.compose.material.icons.outlined.Place
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

/** 按系统版本返回所需运行时权限 */
fun requiredPermissions(): List<String> {
    val list = mutableListOf<String>()
    if (Build.VERSION.SDK_INT >= 31) {
        list += Manifest.permission.BLUETOOTH_SCAN     // neverForLocation
        list += Manifest.permission.BLUETOOTH_CONNECT
    } else {
        list += Manifest.permission.ACCESS_FINE_LOCATION
    }
    list += Manifest.permission.RECORD_AUDIO           // v2：AI 语音会话
    if (Build.VERSION.SDK_INT >= 33) {
        list += Manifest.permission.POST_NOTIFICATIONS
    }
    return list
}

/**
 * 权限引导页：Android 12+ 走 BLUETOOTH_SCAN/CONNECT，低版本走定位权限。
 */
@Composable
fun PermissionsGate(onRecheck: () -> Unit) {
    val context = LocalContext.current
    var deniedOnce by rememberSaveable { mutableStateOf(false) }

    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) {
        deniedOnce = true
        onRecheck()
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Icon(
            imageVector = Icons.Outlined.Bluetooth,
            contentDescription = null,
            modifier = Modifier.size(64.dp),
            tint = MaterialTheme.colorScheme.primary,
        )
        Spacer(Modifier.height(16.dp))
        Text("需要蓝牙与麦克风权限", style = MaterialTheme.typography.headlineSmall, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(8.dp))
        Text(
            "连接 AI-Watch-403 手表需要以下权限。\n扫描权限已声明「绝不用于定位」。",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(20.dp))

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                PermissionRow(
                    icon = { Icon(Icons.Outlined.Place, null, tint = MaterialTheme.colorScheme.primary) },
                    title = if (Build.VERSION.SDK_INT >= 31) "附近的设备（扫描）" else "位置权限（扫描）",
                    desc = if (Build.VERSION.SDK_INT >= 31)
                        "扫描附近的蓝牙设备（BLUETOOTH_SCAN，neverForLocation）"
                    else
                        "Android 11 及以下扫描 BLE 设备需要定位权限",
                )
                if (Build.VERSION.SDK_INT >= 31) {
                    PermissionRow(
                        icon = { Icon(Icons.Outlined.Bluetooth, null, tint = MaterialTheme.colorScheme.primary) },
                        title = "附近的设备（连接）",
                        desc = "连接并通信（BLUETOOTH_CONNECT）",
                    )
                }
                PermissionRow(
                    icon = { Icon(Icons.Outlined.Mic, null, tint = MaterialTheme.colorScheme.primary) },
                    title = "麦克风",
                    desc = "AI 语音会话录音（仅发送到你在设置中配置的识别服务）",
                )
                if (Build.VERSION.SDK_INT >= 33) {
                    PermissionRow(
                        icon = { Icon(Icons.Outlined.Notifications, null, tint = MaterialTheme.colorScheme.primary) },
                        title = "通知",
                        desc = "显示「后台保持连接」的常驻通知（可选，拒绝不影响使用）",
                    )
                }
            }
        }

        Spacer(Modifier.height(20.dp))
        Button(onClick = { launcher.launch(requiredPermissions().toTypedArray()) }) {
            Text("授予权限")
        }
        if (deniedOnce) {
            TextButton(onClick = {
                runCatching {
                    context.startActivity(
                        Intent(
                            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                            Uri.parse("package:${context.packageName}"),
                        ),
                    )
                }
            }) { Text("在系统设置中手动开启") }
        }
    }
}

@Composable
private fun PermissionRow(icon: @Composable () -> Unit, title: String, desc: String) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        icon()
        Column {
            Text(title, style = MaterialTheme.typography.bodyLarge, fontWeight = FontWeight.Medium)
            Text(desc, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
