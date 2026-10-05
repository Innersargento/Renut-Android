package org.renut.android

import android.app.Activity
import android.graphics.BitmapFactory
import android.view.SurfaceView
import android.view.View
import androidx.activity.ComponentActivity
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.EmojiEvents
import androidx.compose.material.icons.filled.Folder
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import java.text.DateFormat
import java.util.Date

private val Background = Color(0xFF101113)
private val Surface = Color(0xFF1C1E21)
private val Accent = Color(0xFFFF9B45)
private val Text = Color(0xFFF4F0EB)
private val Muted = Color(0xFFA8A29C)

object ComposeScreens {
    @JvmStatic fun showMain(activity: MainActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { MainScreen(activity) } } })
    }
    @JvmStatic fun showController(activity: ControllerSettingsActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { ControllerScreen(activity) } } })
    }
    @JvmStatic fun showProbe(activity: ProbeActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { ProbeScreen(activity) } } })
    }
    @JvmStatic fun showRuntime(activity: RuntimeActivity) {
        activity.setContentView(ComposeView(activity).apply { setContent { RenutTheme { RuntimeScreen(activity) } } })
    }
    @JvmStatic fun createGameMenu(activity: RuntimeActivity): View = ComposeView(activity).apply {
        setContent { RenutTheme { GameMenuScreen(activity) } }
    }
}

@Composable private fun RenutTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Accent, onPrimary = Background, background = Background,
            surface = Surface, onSurface = Text, onBackground = Text,
            secondary = Accent, secondaryContainer = Color(0xFF4B301F),
            onSecondaryContainer = Accent, surfaceVariant = Surface,
            onSurfaceVariant = Muted, outline = Color(0xFF3A3B3D)
        ), content = content
    )
}

@Composable private fun MainScreen(activity: MainActivity) {
    var page by remember { mutableIntStateOf(0) }
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(700); tick++ } }
    Scaffold(
        containerColor = Background,
        bottomBar = {
            NavigationBar(containerColor = Surface) {
                NavigationBarItem(selected = page == 0, onClick = { page = 0 },
                    icon = { Icon(Icons.Default.PlayArrow, null) }, label = { Text("Jogar") })
                NavigationBarItem(selected = page == 1, onClick = { page = 1 },
                    icon = { Icon(Icons.Default.Folder, null) }, label = { Text("Saves") })
                NavigationBarItem(selected = page == 2, onClick = { page = 2 },
                    icon = { Icon(Icons.Default.EmojiEvents, null) }, label = { Text("Conquistas") })
                NavigationBarItem(selected = page == 3, onClick = { page = 3 },
                    icon = { Icon(Icons.Default.Settings, null) }, label = { Text("Configurações") })
            }
        }
    ) { padding ->
        when (page) {
            0 -> PlayScreen(activity, Modifier.padding(padding), tick) { page = 2 }
            1 -> SaveScreen(activity, Modifier.padding(padding), tick)
            2 -> AchievementsScreen(activity, Modifier.padding(padding), tick)
            else -> SettingsScreen(activity, Modifier.padding(padding), tick)
        }
    }
}

@Composable private fun SaveScreen(activity: MainActivity, modifier: Modifier, tick: Int) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val saves = rememberUpdatedState(runCatching { activity.uiSaves() }.getOrDefault(emptyList()))
    val visibleSaves = saves.value.filterNot { it.contains("/Headers/") }.ifEmpty { saves.value }
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("SEUS SAVES", "Arquivos do jogo", "Gerencie os saves guardados neste aparelho.") }
        item {
            Panel("Biblioteca") {
                Text(if (visibleSaves.isEmpty()) "Nenhum save encontrado" else "${visibleSaves.size} save(s) encontrado(s)", color = Text, fontWeight = FontWeight.SemiBold)
                Text("Os arquivos são lidos da pasta usada pelo jogo.", color = Muted, fontSize = 13.sp)
            }
        }
        if (visibleSaves.isNotEmpty()) {
            item { SectionTitle("Arquivos") }
            items(visibleSaves) { name -> Panel(null) {
                Text(name.substringAfterLast('/'), color = Text, fontWeight = FontWeight.SemiBold)
                Text("Save do jogo", color = Muted, fontSize = 13.sp)
            } }
        }
        item {
            Panel("Transferir") {
                ActionButton("Exportar saves (.zip)", !activity.uiBusy()) { activity.uiAction("Exportar saves (.zip)") }
                ActionButton("Importar saves (.zip)", !activity.uiBusy(), outlined = true) { activity.uiAction("Importar saves (.zip)") }
                Text("A importação pede confirmação antes de substituir arquivos existentes.", color = Muted, fontSize = 12.sp)
            }
        }
    }
}

@Composable private fun PlayScreen(activity: MainActivity, modifier: Modifier, tick: Int, openAchievements: () -> Unit) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val state = activity.uiGameState()
    val busy = activity.uiBusy()
    val autoStart = activity.uiAutoStart()
    val stamp = Achievements.stamp(activity.filesDir)
    val progress by produceState<Achievements.Snapshot?>(null, stamp) {
        value = withContext(Dispatchers.IO) { Achievements.load(activity.filesDir) }
    }
    // Read from the game on its first launch; until then the card shows a placeholder.
    val cover = remember(stamp) {
        runCatching { BitmapFactory.decodeFile(java.io.File(activity.filesDir, "achievements/title.png").path)?.asImageBitmap() }.getOrNull()
    }
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("RENUT / JOGAR", "Pronto para jogar?", "Sua aventura continua de onde você parou.") }
        item {
            Card(colors = CardDefaults.cardColors(containerColor = Color.Transparent), shape = RoundedCornerShape(24.dp), modifier = Modifier.fillMaxWidth()) {
                Column(
                    Modifier.background(Brush.linearGradient(listOf(Color(0xFF4B301F), Color(0xFF241A14), Surface))).padding(20.dp),
                    verticalArrangement = Arrangement.spacedBy(18.dp)
                ) {
                    Row(horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(84.dp).clip(RoundedCornerShape(18.dp)).background(Color(0x33FF9B45)), contentAlignment = Alignment.Center) {
                            if (cover != null) Image(cover, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
                            else Icon(Icons.Default.PlayArrow, null, tint = Accent, modifier = Modifier.size(40.dp))
                        }
                        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                            Text("Banjo-Kazooie", color = Text, fontWeight = FontWeight.Bold, fontSize = 24.sp)
                            Text("Nuts & Bolts", color = Accent, fontWeight = FontWeight.SemiBold, fontSize = 17.sp)
                            Text(when (state) {
                                2 -> "Edição europeia · jogo completo"
                                1 -> "Edição europeia · só o XEX importado"
                                else -> "Nenhum arquivo importado"
                            }, color = Muted, fontSize = 13.sp)
                        }
                    }
                    Button(
                        onClick = { activity.uiStartGame() }, enabled = state == 2 && !busy,
                        modifier = Modifier.fillMaxWidth().height(60.dp), shape = RoundedCornerShape(16.dp)
                    ) {
                        Icon(Icons.Default.PlayArrow, null, modifier = Modifier.size(28.dp))
                        Spacer(Modifier.width(8.dp))
                        Text("Jogar", fontWeight = FontWeight.Bold, fontSize = 19.sp)
                    }
                    if (state != 2) Text(
                        if (state == 1) "Importe a imagem ISO completa para ter os arquivos que o jogo precisa."
                        else "Importe a imagem ISO do jogo abaixo para começar.",
                        color = Muted, fontSize = 13.sp)
                    else Text("Experimental: o desempenho e a compatibilidade ainda estão em desenvolvimento.", color = Muted, fontSize = 12.sp)
                    if (state == 2 && activity.uiRuntimeOption(RuntimeOptions.CHEATS, "false") == "true")
                        Text("Cheats ligados nesta partida", color = Accent, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                }
            }
        }
        progress?.let { data ->
            item {
                Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(20.dp),
                    modifier = Modifier.fillMaxWidth().clickable { openAchievements() }) {
                    Row(Modifier.padding(18.dp), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.EmojiEvents, null, tint = Accent, modifier = Modifier.size(32.dp))
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text("${data.unlocked.size} de ${data.count()} conquistas", color = Text, fontWeight = FontWeight.SemiBold)
                            LinearProgressIndicator(
                                progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                                modifier = Modifier.fillMaxWidth().height(6.dp).clip(RoundedCornerShape(3.dp)),
                                color = Accent, trackColor = Color(0xFF3A3B3D))
                        }
                        Text("${data.earnedGamerscore} G", color = Accent, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
        item { CheatsPanel(activity, !busy) }
        item {
            Panel("Ao abrir o app") {
                ToggleRow("Iniciar direto no jogo", autoStart, !busy) { activity.uiSetAutoStart(it) }
                Text("O jogo começa sem passar por esta tela. Para voltar aqui, saia do jogo pelo menu.", color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel("Arquivos do jogo") {
                ActionButton("Importar imagem ISO", !busy, outlined = true) { activity.uiAction("Importar imagem ISO") }
                ActionButton("Importar apenas o XEX", !busy, outlined = true) { activity.uiAction("Importar apenas o XEX") }
                if (busy) Text(activity.uiStatus(), color = Muted, fontSize = 13.sp, maxLines = 4, overflow = TextOverflow.Ellipsis)
                if (activity.uiCanCancel()) ActionButton("Cancelar importação", true, outlined = true) { activity.uiCancelImport() }
                Text("A importação da ISO precisa de espaço livre para a imagem e os arquivos extraídos.", color = Muted, fontSize = 12.sp)
            }
        }
    }
}

private val CHEAT_LABELS = listOf(
    "renut_cheat_infinite_health" to "Vida infinita",
    "renut_cheat_infinite_fuel_ammo" to "Combustível e munição infinitos",
    "renut_cheat_infinite_parts" to "Peças infinitas",
    "renut_cheat_no_timer" to "Sem limite de tempo nas missões",
    "renut_cheat_no_notes_spent" to "Notas não são gastas",
    "renut_cheat_vehicles_anywhere" to "Trocar e montar veículo em qualquer lugar",
    "renut_cheat_extended_build_range" to "Área de montagem maior (Mumbo's Motors)")
private val SKIN_LABELS = listOf("default" to "Normal", "robot" to "Robô", "tuxedo" to "Smoking")

/** Cheats of the next game start. The switches live in renut.toml, which the runtime reads at launch. */
@Composable private fun CheatsPanel(activity: MainActivity, enabled: Boolean) {
    var master by remember { mutableStateOf(activity.uiRuntimeOption(RuntimeOptions.CHEATS, "false") == "true") }
    val flags = remember {
        mutableStateMapOf<String, Boolean>().apply {
            CHEAT_LABELS.forEach { (key, _) -> put(key, activity.uiRuntimeOption(key, "false") == "true") }
        }
    }
    var skin by remember { mutableStateOf(activity.uiRuntimeOption(RuntimeOptions.CHEAT_SKIN, "default")) }
    Panel("Cheats") {
        ToggleRow("Aplicar cheats ao jogar", master, enabled) {
            master = it
            activity.uiSetRuntimeOption(RuntimeOptions.CHEATS, it.toString())
        }
        if (master) {
            CHEAT_LABELS.forEach { (key, label) ->
                ToggleRow(label, flags[key] == true, enabled) {
                    flags[key] = it
                    activity.uiSetRuntimeOption(key, it.toString())
                }
            }
            SingleChoiceSelector("Visual do Banjo", SKIN_LABELS.firstOrNull { it.first == skin }?.second ?: "Normal",
                SKIN_LABELS.map { it.second }) { label ->
                val value = SKIN_LABELS.first { it.second == label }.first
                skin = value
                activity.uiSetRuntimeOption(RuntimeOptions.CHEAT_SKIN, value)
            }
        }
        Text("Valem a partir da próxima partida. Peças infinitas altera as contagens de peças do seu save.", color = Muted, fontSize = 12.sp)
    }
}

@Composable private fun AchievementsScreen(activity: MainActivity, modifier: Modifier, tick: Int) {
    // The runtime rewrites these files while the game runs; reload only when they change.
    @Suppress("UNUSED_VARIABLE") val update = tick
    val stamp = Achievements.stamp(activity.filesDir)
    val loaded = remember { mutableStateOf(false) }
    val snapshot by produceState<Achievements.Snapshot?>(null, stamp) {
        value = withContext(Dispatchers.IO) { Achievements.load(activity.filesDir) }
        loaded.value = true
    }
    val data = snapshot
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("CONQUISTAS", "Suas conquistas", "As conquistas desbloqueadas no jogo, neste aparelho.") }
        if (data == null) {
            if (loaded.value) item {
                Panel("Ainda sem lista") {
                    Text("A lista de conquistas é lida do seu jogo na primeira vez em que ele inicia. Abra o jogo uma vez e volte aqui.", color = Muted, fontSize = 14.sp)
                }
            }
        } else {
            item {
                Panel("Progresso") {
                    Text("${data.unlocked.size} de ${data.count()} desbloqueadas", color = Text, fontWeight = FontWeight.SemiBold)
                    LinearProgressIndicator(
                        progress = { if (data.count() == 0) 0f else data.unlocked.size / data.count().toFloat() },
                        modifier = Modifier.fillMaxWidth().height(8.dp).clip(RoundedCornerShape(4.dp)),
                        color = Accent, trackColor = Color(0xFF3A3B3D))
                    Text("${data.earnedGamerscore} de ${data.gamerscore} G", color = Muted, fontSize = 13.sp)
                }
            }
            if (data.unlocked.isNotEmpty()) {
                item { SectionTitle("Desbloqueadas") }
                items(data.unlocked, key = { it.id }) { AchievementRow(it) }
            }
            if (data.locked.isNotEmpty()) {
                item { SectionTitle("Bloqueadas") }
                items(data.locked, key = { it.id }) { AchievementRow(it) }
            }
        }
    }
}

@Composable private fun AchievementRow(item: Achievements.Item) {
    val unlocked = item.unlocked()
    val icon = remember(item.icon) { item.icon?.let { runCatching { BitmapFactory.decodeFile(it.path)?.asImageBitmap() }.getOrNull() } }
    val gray = remember { ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(0f) }) }
    Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(16.dp), modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(14.dp), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(56.dp).clip(RoundedCornerShape(12.dp)).background(Color(0xFF34363A)).alpha(if (unlocked) 1f else 0.45f)) {
                if (icon != null) Image(icon, null, Modifier.fillMaxSize(), contentScale = ContentScale.Crop, colorFilter = if (unlocked) null else gray)
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(item.label, color = if (unlocked) Text else Muted, fontWeight = FontWeight.Bold, fontSize = 16.sp)
                Text(item.shownDescription(), color = Muted, fontSize = 13.sp)
                if (unlocked) Text(
                    "Desbloqueada em " + DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(item.unlockedAt)),
                    color = Accent, fontSize = 12.sp)
            }
            if (item.gamerscore > 0) Text("${item.gamerscore} G", color = if (unlocked) Accent else Muted, fontWeight = FontWeight.Bold, fontSize = 14.sp)
        }
    }
}

@Composable private fun SettingsScreen(activity: MainActivity, modifier: Modifier, tick: Int) {
    @Suppress("UNUSED_VARIABLE") val update = tick
    val driver = activity.uiDriver()
    val status = activity.uiStatus()
    val busy = activity.uiBusy()
    val canCancel = activity.uiCanCancel()
    val fpsEnabled = activity.uiFpsEnabled()
    val controlsEnabled = activity.uiShowControls()
    val oneUiEnabled = activity.uiOneUiEnabled()
    val tuMode = activity.uiTuMode()
    val ir3Mode = activity.uiIr3Mode()
    val logLevel = activity.uiRuntimeOption("log_level", "info")
    val shaderAsync = activity.uiRuntimeOption("renut_plume_async_pipelines", "false") == "true"
    val uncapFps = activity.uiRuntimeOption("renut_plume_uncap_fps", "false") == "true"
    LazyColumn(modifier.fillMaxSize(), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        item { Header("RENUT / ANDROID", "Configurações", "Jogo, vídeo, controles e diagnóstico.") }
        item {
            Panel("Display") {
                ToggleRow("Contador de FPS", fpsEnabled, !busy) { activity.uiSetFps(it) }
                ToggleRow("Destravar FPS", uncapFps, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_uncap_fps", it.toString())
                }
                Text("Usa o modo de apresentação imediato e a maior frequência da tela na próxima partida. Pode alterar o ritmo de partes do jogo.", color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel("Controles") {
                ToggleRow("Mostrar controles na tela", controlsEnabled, !busy) { activity.uiSetShowControls(it) }
                ActionButton("Configurar controle externo", !busy, outlined = true) { activity.uiAction("Configurar controle externo") }
            }
        }
        item {
            Panel("Avançado") {
                InfoRow("Driver", driver)
                ActionButton("Importar driver Adreno (.zip)", !busy, outlined = true) { activity.uiAction("Importar driver Adreno (.zip)") }
                ActionButton("Usar driver do sistema", !busy, outlined = true) { activity.uiAction("Usar driver do sistema") }
                ToggleRow("One UI fix", oneUiEnabled, !busy) { activity.uiSetOneUi(it) }
                MultiModeSelector("TU_DEBUG", tuMode, listOf("perf", "sysmem", "gmem")) { activity.uiSetTuMode(it) }
                MultiModeSelector("IR3_SHADER_DEBUG", ir3Mode, listOf("nopreamble", "noearlypreamble", "nouboopt", "nodescprefetch")) { activity.uiSetIr3Mode(it) }
                ActionButton("Apagar cache de shaders", !busy, outlined = true) { activity.uiAction("Apagar cache de shaders") }
            }
        }
        item {
            Panel("Opções do runtime") {
                Text("Aplicadas na próxima inicialização do jogo.", color = Muted, fontSize = 13.sp)
                SingleChoiceSelector("Nível do log", logLevel,
                    listOf("trace", "debug", "info", "warn", "error", "critical", "off")) {
                    activity.uiSetRuntimeOption("log_level", it)
                }
                ToggleRow("Compilação assíncrona de shaders e pipelines", shaderAsync, !busy) {
                    activity.uiSetRuntimeOption("renut_plume_async_pipelines", it.toString())
                }
                Text("Compila pipelines novos em segundo plano. O primeiro desenho pode faltar até ficar pronto; desenhos de memexport continuam síncronos.", color = Muted, fontSize = 12.sp)
            }
        }
        item {
            Panel("Testes e diagnóstico") {
                Text(status, color = Muted, fontSize = 13.sp, maxLines = 8, overflow = TextOverflow.Ellipsis)
                if (canCancel) ActionButton("Cancelar importação", true, outlined = true) { activity.uiCancelImport() }
                ActionButton("Validar runtime ARM64", !busy, outlined = true) { activity.uiAction("Validar runtime ARM64") }
                ActionButton("Testar runtime com Plume", !busy, outlined = true) { activity.uiAction("Testar runtime com Plume") }
                ActionButton("Testar Plume no aparelho", !busy, outlined = true) { activity.uiAction("Testar Plume no aparelho") }
                ActionButton("Exportar diagnóstico", !busy, outlined = true) { activity.uiAction("Exportar diagnóstico") }
            }
        }
        item { Text("reNut · prévia de desenvolvimento", color = Muted, fontSize = 12.sp) }
    }
}

@Composable private fun Header(eyebrow: String, title: String, subtitle: String) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(eyebrow, color = Accent, fontWeight = FontWeight.Bold, fontSize = 12.sp, letterSpacing = 1.sp)
        Text(title, color = Text, fontWeight = FontWeight.Bold, fontSize = 32.sp)
        Text(subtitle, color = Muted, fontSize = 14.sp)
    }
}

@Composable private fun SectionTitle(title: String) { Text(title, color = Accent, fontWeight = FontWeight.Bold, fontSize = 17.sp) }

@Composable private fun Panel(title: String?, content: @Composable ColumnScope.() -> Unit) {
    Card(colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(20.dp), modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (title != null) Text(title, color = Text, fontWeight = FontWeight.Bold, fontSize = 19.sp)
            content()
        }
    }
}

@Composable private fun InfoRow(label: String, value: String) {
    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
        Text(label, color = Accent, fontSize = 12.sp, fontWeight = FontWeight.Bold)
        Text(value, color = Text, fontSize = 14.sp)
    }
}

@Composable private fun ToggleRow(label: String, checked: Boolean, enabled: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Text, modifier = Modifier.weight(1f))
        Switch(checked = checked, enabled = enabled, onCheckedChange = onCheckedChange)
    }
}

@Composable private fun MultiModeSelector(label: String, selected: String, choices: List<String>, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    var local by remember { mutableStateOf(selected) }
    Box {
        OutlinedButton(onClick = { local = selected; expanded = true }, modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(12.dp)) {
            Text("$label: ${(if (expanded) local else selected).ifEmpty { "Desativado" }.replace(",", ", ")}")
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            DropdownMenuItem(text = { Text("Desativado") }, leadingIcon = {
                Checkbox(checked = local.isEmpty(), onCheckedChange = null)
            }, onClick = { local = ""; onSelect("") })
            choices.forEach { choice ->
                DropdownMenuItem(text = { Text(choice) }, leadingIcon = {
                    Checkbox(checked = choice in local.split(',').filter { it.isNotEmpty() }, onCheckedChange = null)
                }, onClick = {
                    val values = local.split(',').filter { it.isNotEmpty() }.toMutableSet()
                    if (!values.add(choice)) values.remove(choice)
                    val updated = choices.filter { it in values }.joinToString(",")
                    local = updated
                    onSelect(updated)
                })
            }
        }
    }
}

@Composable private fun SingleChoiceSelector(label: String, selected: String, choices: List<String>, onSelect: (String) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    Box {
        OutlinedButton(onClick = { expanded = true }, modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(12.dp)) {
            Text("$label: $selected")
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            choices.forEach { choice ->
                DropdownMenuItem(text = { Text(choice) }, onClick = { expanded = false; onSelect(choice) })
            }
        }
    }
}

@Composable private fun ActionButton(label: String, enabled: Boolean, outlined: Boolean = false, onClick: () -> Unit) {
    if (outlined) OutlinedButton(onClick, Modifier.fillMaxWidth(), enabled = enabled, shape = RoundedCornerShape(12.dp)) { Text(label) }
    else Button(onClick, Modifier.fillMaxWidth(), enabled = enabled, shape = RoundedCornerShape(12.dp)) { Text(label) }
}

@Composable private fun GameMenuScreen(activity: RuntimeActivity) {
    var controlsVisible by remember { mutableStateOf(activity.uiGameControlsVisible()) }
    Box(Modifier.fillMaxSize().background(Color(0xBF000000)).clickable { }, contentAlignment = Alignment.Center) {
        Column(Modifier.widthIn(max = 420.dp).fillMaxWidth().padding(20.dp)) {
            Panel(null) {
                Header("RENUT / JOGO", "Menu do jogo", "Ajuste os controles ou volte à biblioteca.")
                ToggleRow("Mostrar controles na tela", controlsVisible, true) {
                    controlsVisible = it
                    activity.uiSetGameControlsVisible(it)
                }
                ActionButton("Continuar", true) { activity.uiCloseGameMenu() }
                ActionButton("Sair para o menu principal", true, outlined = true) { activity.finish() }
            }
        }
    }
}

@Composable private fun ControllerScreen(activity: ControllerSettingsActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    var chooseDevice by remember { mutableStateOf(false) }
    val devices = activity.uiDevices()
    Scaffold(containerColor = Background, topBar = {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { activity.finish() }) { Icon(Icons.AutoMirrored.Filled.ArrowBack, "Voltar") }
            Text("Controle externo", color = Text, fontWeight = FontWeight.Bold, fontSize = 21.sp)
        }
    }) { padding ->
        LazyColumn(Modifier.fillMaxSize().padding(padding), contentPadding = PaddingValues(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            item { Text("Conecte um controle USB ou Bluetooth. O mapeamento é salvo para cada aparelho.", color = Muted) }
            item {
                Panel("Dispositivo") {
                    ActionButton(devices.getOrNull(activity.uiSelected()) ?: "Nenhum controle conectado", true, outlined = true) { chooseDevice = true }
                    ActionButton("Atualizar lista de controles", true, outlined = true) { activity.uiRefresh(); tick++ }
                }
            }
            item {
                Panel("Mapeamento") {
                    Text(activity.uiStatus(), color = Muted, fontSize = 14.sp)
                    ActionButton("Mapear automaticamente", devices.isNotEmpty()) { activity.uiAutomatic(); tick++ }
                    if (activity.uiPending() >= 0) ActionButton("Cancelar captura", true, outlined = true) { activity.uiCancel(); tick++ }
                }
            }
            items(activity.uiBindings().withIndex().toList()) { (index, label) ->
                Card(Modifier.fillMaxWidth().clickable { activity.uiCapture(index); tick++ },
                    colors = CardDefaults.cardColors(containerColor = Surface), shape = RoundedCornerShape(14.dp)) {
                    Text(label, Modifier.padding(16.dp), color = if (activity.uiPending() == index) Accent else Text)
                }
            }
        }
    }
    if (chooseDevice) AlertDialog(onDismissRequest = { chooseDevice = false },
        title = { Text("Selecionar controle") },
        text = { Column { devices.forEachIndexed { index, name ->
            Text(name, Modifier.fillMaxWidth().clickable { activity.uiSelect(index); chooseDevice = false; tick++ }.padding(12.dp))
        } } },
        confirmButton = { TextButton(onClick = { chooseDevice = false }) { Text("Fechar") } })
}

@Composable private fun ProbeScreen(activity: ProbeActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    Column(Modifier.fillMaxSize().background(Background).padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Header("TESTE GRÁFICO", "Plume", "Apresentação Vulkan no aparelho")
        Panel("Resultado") { Text(activity.uiStatus(), color = Text, fontSize = 13.sp) }
        AndroidView(factory = { SurfaceView(activity).also(activity::attachComposeSurface) }, modifier = Modifier.fillMaxWidth().weight(1f))
        ActionButton("Voltar à biblioteca", true) { activity.finish() }
    }
}

@Composable private fun RuntimeScreen(activity: RuntimeActivity) {
    var tick by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) { while (true) { delay(500); tick++ } }
    @Suppress("UNUSED_VARIABLE") val refresh = tick
    Column(Modifier.fillMaxSize().background(Background).padding(20.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Header("TESTE DO RUNTIME", if (activity.uiGraphics()) "RexGlue + Plume" else "Runtime ARM64", "Validação experimental")
        Panel("Resultado") { Text(activity.uiStatus(), color = Text, fontSize = 13.sp) }
        if (activity.uiGraphics()) AndroidView(factory = { SurfaceView(activity).also(activity::attachComposeSurface) }, modifier = Modifier.fillMaxWidth().weight(1f))
        else Spacer(Modifier.weight(1f))
        ActionButton("Voltar", true) { activity.finish() }
    }
}
