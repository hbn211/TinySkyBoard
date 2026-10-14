#include "i18n.h"

// Setup-mode help page (http://192.168.4.1/ajuda).
// Keep it in sync with the real behavior: button in main.c, LED in led.c/net.c/portal.c,
// message cycle in ui.c, commands in telegram.c.

#define HELP_STYLE                                                                                   \
    "<style>"                                                                                        \
    "h2{font-size:1.05em;color:#4fc3f7;margin:26px 0 8px}"                                           \
    "ul{padding-left:0;list-style:none;margin:0}li{margin:8px 0;line-height:1.4}"                    \
    "p{line-height:1.45;color:#c8d0de}code{background:#161d2e;padding:1px 6px;border-radius:5px}"    \
    "b{color:#fff}.k{color:#8a94a8;font-size:.9em}"                                                  \
    ".led{display:inline-block;width:14px;height:14px;border-radius:50%;vertical-align:-2px;"        \
    "margin-right:8px;box-shadow:0 0 8px currentColor}"                                              \
    ".bl{animation:bl .3s steps(2) 3}"                      \
    "@keyframes bl{50%{opacity:0}}"                 \
    "</style>"

// LED with the firmware's color
#define LED(rgb) "<span class=led style='background:#" rgb ";color:#" rgb "'></span>"

static const char HELP_PT[] =
    HELP_STYLE
    "<h1>Como funciona o TinySkyBoard</h1>"
    "<p>Um relógio com clima que também mostra mensagens que você manda pelo Telegram.</p>"

    "<h2>A tela</h2><ul>"
    "<li><b>Barra de cima:</b> rede Wi-Fi e força do sinal, ou avisos (conectando, sem Wi-Fi...).</li>"
    "<li><b>Relógio e data:</b> acertados pela internet; o fuso horário vem da cidade.</li>"
    "<li><b>Clima agora:</b> temperatura, céu, umidade e vento. Atualiza a cada 10 min.</li>"
    "<li><b>Previsão:</b> hoje e os próximos 4 dias, com máxima (branco) e mínima (cinza).</li>"
    "<li><b>Mensagens:</b> uma mensagem nova aparece na hora e fica 20 s. Até 5 mensagens (mais os "
    "agendamentos no horário) ficam ativas: o painel aparece 10 s e depois cada mensagem 6 s.</li></ul>"

    "<h2>Os botões</h2>"
    "<p>Ficam na borda da USB. Com a tela de frente e a USB para baixo: <b>esquerda = controle</b> (BOOT), "
    "<b>direita = reset</b>.</p><ul>"
    "<li><b>Clique rápido no controle:</b> mostra a mensagem ativa nº 1; cada novo clique vai para a próxima, "
    "até voltar ao painel. Sem clicar por 10 s, volta ao normal.</li>"
    "<li><b>Segurar 2 s numa mensagem:</b> apaga essa mensagem (uma barra vermelha enche enquanto você "
    "segura; soltando antes, nada é apagado). Agendamento que se repete só some até a próxima vez.</li>"
    "<li><b>Segurar 5 s no painel:</b> entra no modo de configuração. No modo de configuração, segurar 5 s "
    "sai sem salvar.</li>"
    "<li class=k>O reset só reinicia o painel. A posição da tela e a unidade (°C/°F) são escolhidas na "
    "configuração.</li></ul>"

    "<h2>O LED de trás</h2><ul>"
    "<li>" LED("2f6bff") "<b>Azul fixo:</b> conectando no Wi-Fi.</li>"
    "<li>" LED("22c55e") "<b>Verde fixo:</b> conectado, buscando o clima.</li>"
    "<li>" LED("ffd54f") "<b>Fixo na cor do clima:</b> tudo certo. A cor é a do clima de agora:</li>"
    "<li class=k>" LED("ffd54f") "céu limpo &nbsp;" LED("ffe082") "parcialmente nublado &nbsp;"
    LED("90a4ae") "nublado &nbsp;" LED("b0bec5") "neblina<br>"
    LED("81d4fa") "garoa &nbsp;" LED("42a5f5") "chuva &nbsp;" LED("1e88e5") "pancadas &nbsp;"
    LED("ffffff") "neve &nbsp;" LED("ab47bc") "tempestade</li>"
    "<li><span class='led bl' style='background:#fff;color:#fff'></span><b>3 piscadas brancas:</b> "
    "chegou mensagem.</li>"
    "<li>" LED("ff3b30") "<b>Vermelho fixo:</b> o Wi-Fi caiu. O painel tenta reconectar sozinho.</li>"
    "<li>" LED("ff8000") "<b>Laranja fixo:</b> modo de configuração (esta página).</li></ul>"

    "<h2>Brilho</h2><ul>"
    "<li>Na configuração há um brilho de <b>dia</b> e um de <b>noite</b> para a tela e para o LED "
    "(LED em 0% = apagado).</li>"
    "<li>O painel troca sozinho no <b>nascer e no pôr do sol</b> da sua cidade, aos poucos, em 30 min. "
    "Tela e LED seguem os mesmos horários.</li></ul>"

    "<h2>Modo de configuração</h2><ul>"
    "<li>Entra segurando o BOOT por 5 s, ou sozinho quando não consegue conectar no Wi-Fi em 45 s "
    "(por exemplo, quando você muda de lugar).</li>"
    "<li>O painel cria a rede <b>TinySkyBoard</b> e mostra um QR code. Conecte nela e abra "
    "<code>192.168.4.1</code>.</li>"
    "<li>Sem salvar nada, ele reinicia sozinho depois de 15 min e tenta o Wi-Fi de novo.</li>"
    "<li class=k>Para a cidade, basta o nome: coordenadas e fuso horário são descobertos sozinhos.</li></ul>"

    "<h2>Telegram</h2>"
    "<p>Crie um bot com o <b>@BotFather</b> (<code>/newbot</code>) e cole o token na configuração. "
    "O primeiro chat que falar com o bot vira o dono; mensagens de outras pessoas são ignoradas.</p><ul>"
    "<li><b>Qualquer texto</b> aparece no painel e o bot responde ✓.</li>"
    "<li><code>/agendar</code> programa uma mensagem: hora, duração e se repete.</li>"
    "<li><code>/agenda</code> lista o que está agendado; <code>/cancelar N</code> apaga o item N.</li>"
    "<li><code>/limpar</code> tira a mensagem da tela.</li>"
    "<li><code>/status</code> mostra se está ligado, há quanto tempo, Wi-Fi e clima. Sem resposta = painel "
    "fora do ar.</li>"
    "<li><code>/ajuda</code> lista os comandos.</li></ul>"
    "<p>O bot também avisa sozinho quando o painel liga (e o motivo, como falta de energia) e quando a "
    "internet volta depois de cair.</p>"
    "<p class=k>Emoji ainda não aparecem na tela; acentos sim.</p>";

static const char HELP_EN[] =
    HELP_STYLE
    "<h1>How TinySkyBoard works</h1>"
    "<p>A clock with weather that also shows messages you send over Telegram.</p>"

    "<h2>The screen</h2><ul>"
    "<li><b>Top bar:</b> Wi-Fi network and signal strength, or notices (connecting, no Wi-Fi...).</li>"
    "<li><b>Clock and date:</b> set from the internet; the time zone comes from the city.</li>"
    "<li><b>Weather now:</b> temperature, sky, humidity and wind. Updates every 10 min.</li>"
    "<li><b>Forecast:</b> today and the next 4 days, with high (white) and low (gray).</li>"
    "<li><b>Messages:</b> a new message shows up right away and stays for 20 s. Up to 5 messages (plus "
    "schedules that are on) stay active: the panel shows for 10 s, then each message for 6 s.</li></ul>"

    "<h2>The buttons</h2>"
    "<p>They sit on the USB edge. With the screen facing you and the USB down: <b>left = control</b> (BOOT), "
    "<b>right = reset</b>.</p><ul>"
    "<li><b>Quick press on control:</b> shows active message #1; each new press goes to the next one, "
    "until it's back to the panel. After 10 s without pressing, it goes back to normal.</li>"
    "<li><b>Hold 2 s on a message:</b> deletes that message (a red bar fills while you hold; let go "
    "earlier and nothing is deleted). A repeating schedule is only hidden until next time.</li>"
    "<li><b>Hold 5 s on the panel:</b> enters setup mode. In setup mode, holding 5 s exits without "
    "saving.</li>"
    "<li class=k>Reset only restarts the panel. Screen position and unit (°C/°F) are chosen in the setup.</li></ul>"

    "<h2>The LED on the back</h2><ul>"
    "<li>" LED("2f6bff") "<b>Solid blue:</b> connecting to Wi-Fi.</li>"
    "<li>" LED("22c55e") "<b>Solid green:</b> connected, getting the weather.</li>"
    "<li>" LED("ffd54f") "<b>Solid in the weather color:</b> all good. The color is the current weather:</li>"
    "<li class=k>" LED("ffd54f") "clear sky &nbsp;" LED("ffe082") "partly cloudy &nbsp;"
    LED("90a4ae") "cloudy &nbsp;" LED("b0bec5") "fog<br>"
    LED("81d4fa") "drizzle &nbsp;" LED("42a5f5") "rain &nbsp;" LED("1e88e5") "showers &nbsp;"
    LED("ffffff") "snow &nbsp;" LED("ab47bc") "thunderstorm</li>"
    "<li><span class='led bl' style='background:#fff;color:#fff'></span><b>3 white blinks:</b> "
    "a message arrived.</li>"
    "<li>" LED("ff3b30") "<b>Solid red:</b> Wi-Fi is down. The panel reconnects by itself.</li>"
    "<li>" LED("ff8000") "<b>Solid orange:</b> setup mode (this page).</li></ul>"

    "<h2>Brightness</h2><ul>"
    "<li>The setup has a <b>day</b> and a <b>night</b> brightness for the screen and for the LED "
    "(LED at 0% = off).</li>"
    "<li>The panel switches by itself at <b>sunrise and sunset</b> in your city, gradually over 30 min. "
    "Screen and LED follow the same times.</li></ul>"

    "<h2>Setup mode</h2><ul>"
    "<li>Enter by holding BOOT for 5 s, or automatically when it can't connect to Wi-Fi within 45 s "
    "(for example, after you move).</li>"
    "<li>The panel creates the <b>TinySkyBoard</b> network and shows a QR code. Join it and open "
    "<code>192.168.4.1</code>.</li>"
    "<li>If nothing is saved, it restarts after 15 min and tries the Wi-Fi again.</li>"
    "<li class=k>For the city, the name is enough: coordinates and time zone are found automatically.</li></ul>"

    "<h2>Telegram</h2>"
    "<p>Create a bot with <b>@BotFather</b> (<code>/newbot</code>) and paste the token in the setup. "
    "The first chat that talks to the bot becomes the owner; messages from anyone else are ignored.</p><ul>"
    "<li><b>Any text</b> shows up on the panel and the bot replies ✓.</li>"
    "<li><code>/schedule</code> schedules a message: time, duration and repeat.</li>"
    "<li><code>/list</code> shows what is scheduled; <code>/cancel N</code> deletes item N.</li>"
    "<li><code>/clear</code> removes the message from the screen.</li>"
    "<li><code>/status</code> shows that it's on, for how long, Wi-Fi and weather. No reply = panel offline.</li>"
    "<li><code>/help</code> lists the commands.</li></ul>"
    "<p>The bot also tells you on its own when the panel powers on (and why, like a power cut) and when "
    "the internet comes back after a drop.</p>"
    "<p class=k>Emoji don't show on the screen yet; accents do.</p>";

static const char HELP_ES[] =
    HELP_STYLE
    "<h1>Cómo funciona el TinySkyBoard</h1>"
    "<p>Un reloj con el clima que también muestra los mensajes que envías por Telegram.</p>"

    "<h2>La pantalla</h2><ul>"
    "<li><b>Barra de arriba:</b> red Wi-Fi y fuerza de la señal, o avisos (conectando, sin Wi-Fi...).</li>"
    "<li><b>Reloj y fecha:</b> se ajustan por internet; la zona horaria viene de la ciudad.</li>"
    "<li><b>Clima ahora:</b> temperatura, cielo, humedad y viento. Se actualiza cada 10 min.</li>"
    "<li><b>Pronóstico:</b> hoy y los próximos 4 días, con máxima (blanco) y mínima (gris).</li>"
    "<li><b>Mensajes:</b> un mensaje nuevo aparece al instante y se queda 20 s. Hasta 5 mensajes (más las "
    "programaciones en su horario) quedan activos: el panel aparece 10 s y luego cada mensaje 6 s.</li></ul>"

    "<h2>Los botones</h2>"
    "<p>Están en el borde del USB. Con la pantalla de frente y el USB hacia abajo: <b>izquierda = control</b> "
    "(BOOT), <b>derecha = reset</b>.</p><ul>"
    "<li><b>Clic rápido en control:</b> muestra el mensaje activo nº 1; cada clic pasa al siguiente, hasta "
    "volver al panel. Sin clics durante 10 s, vuelve a lo normal.</li>"
    "<li><b>Mantener 2 s en un mensaje:</b> borra ese mensaje (una barra roja se llena mientras mantienes; "
    "si sueltas antes, no se borra nada). Una programación que se repite solo se oculta hasta la próxima vez.</li>"
    "<li><b>Mantener 5 s en el panel:</b> entra al modo de configuración. En el modo de configuración, "
    "mantener 5 s sale sin guardar.</li>"
    "<li class=k>El reset solo reinicia el panel. La posición de la pantalla y la unidad (°C/°F) se eligen "
    "en la configuración.</li></ul>"

    "<h2>El LED de atrás</h2><ul>"
    "<li>" LED("2f6bff") "<b>Azul fijo:</b> conectando al Wi-Fi.</li>"
    "<li>" LED("22c55e") "<b>Verde fijo:</b> conectado, buscando el clima.</li>"
    "<li>" LED("ffd54f") "<b>Fijo en el color del clima:</b> todo bien. El color es el del clima actual:</li>"
    "<li class=k>" LED("ffd54f") "despejado &nbsp;" LED("ffe082") "parcialmente nublado &nbsp;"
    LED("90a4ae") "nublado &nbsp;" LED("b0bec5") "niebla<br>"
    LED("81d4fa") "llovizna &nbsp;" LED("42a5f5") "lluvia &nbsp;" LED("1e88e5") "chubascos &nbsp;"
    LED("ffffff") "nieve &nbsp;" LED("ab47bc") "tormenta</li>"
    "<li><span class='led bl' style='background:#fff;color:#fff'></span><b>3 parpadeos blancos:</b> "
    "llegó un mensaje.</li>"
    "<li>" LED("ff3b30") "<b>Rojo fijo:</b> se cayó el Wi-Fi. El panel se reconecta solo.</li>"
    "<li>" LED("ff8000") "<b>Naranja fijo:</b> modo de configuración (esta página).</li></ul>"

    "<h2>Brillo</h2><ul>"
    "<li>En la configuración hay un brillo de <b>día</b> y uno de <b>noche</b> para la pantalla y para "
    "el LED (LED en 0% = apagado).</li>"
    "<li>El panel cambia solo al <b>amanecer y al atardecer</b> de tu ciudad, poco a poco, en 30 min. "
    "Pantalla y LED siguen los mismos horarios.</li></ul>"

    "<h2>Modo de configuración</h2><ul>"
    "<li>Se entra manteniendo BOOT 5 s, o solo cuando no logra conectarse al Wi-Fi en 45 s "
    "(por ejemplo, cuando cambias de lugar).</li>"
    "<li>El panel crea la red <b>TinySkyBoard</b> y muestra un código QR. Conéctate y abre "
    "<code>192.168.4.1</code>.</li>"
    "<li>Si no se guarda nada, se reinicia a los 15 min y vuelve a probar el Wi-Fi.</li>"
    "<li class=k>Para la ciudad basta el nombre: las coordenadas y la zona horaria se detectan solas.</li></ul>"

    "<h2>Telegram</h2>"
    "<p>Crea un bot con <b>@BotFather</b> (<code>/newbot</code>) y pega el token en la configuración. "
    "El primer chat que hable con el bot se vuelve el dueño; los mensajes de otras personas se ignoran.</p><ul>"
    "<li><b>Cualquier texto</b> aparece en el panel y el bot responde ✓.</li>"
    "<li><code>/programar</code> programa un mensaje: hora, duración y si se repite.</li>"
    "<li><code>/lista</code> muestra lo programado; <code>/cancelar N</code> borra el elemento N.</li>"
    "<li><code>/borrar</code> quita el mensaje de la pantalla.</li>"
    "<li><code>/estado</code> muestra si está encendido, desde cuándo, el Wi-Fi y el clima. Sin respuesta = "
    "panel sin conexión.</li>"
    "<li><code>/ayuda</code> lista los comandos.</li></ul>"
    "<p>El bot también avisa solo cuando el panel se enciende (y el motivo, como un corte de luz) y cuando "
    "vuelve internet después de caerse.</p>"
    "<p class=k>Los emoji aún no aparecen en la pantalla; los acentos sí.</p>";

const char *portal_help_html(uint8_t lang)
{
    static const char *const PAGES[LANG_COUNT] = {
        [LANG_PT] = HELP_PT,
        [LANG_EN] = HELP_EN,
        [LANG_ES] = HELP_ES,
    };
    return PAGES[lang < LANG_COUNT ? lang : LANG_EN];
}
