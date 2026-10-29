# 5) كود الـ ESP32

الملفات في `firmware/NeuralNexusScreen/`:

| الملف | وظيفته |
|---|---|
| `NeuralNexusScreen.ino` | البرنامج الأساسي: الواي فاي، وسيرفر الويب، والـ API، والاتصال بالشاشة، والرسايل والجدولة |
| `screen_text.h` | تجهيز الكلام: UTF-8، وتشبيك العربي، والترتيب من اليمين للشمال، والتقسيم على سطور، والتوسيط |
| `arabic_tables.h` | جدول أشكال الحروف العربي (أول ووسط وآخر الكلمة ولوحده) |
| `app.html` → `app_html.h` | صفحة الويب الأساسية، متخزنة جوه الـ ESP |
| `diag.html` → `diag_html.h` | صفحة الفحص والتقرير (`/diag`) |

## الحرق (Arduino IDE)

1. نزّل **Arduino IDE 2.x**، وضيف الـ ESP32 boards: في Board Manager دوّر على "esp32 by Espressif"، والنسخة اللي اشتغلنا بيها **3.3.x**.
2. افتح `NeuralNexusScreen.ino`. لازم كل ملفات `.h` تكون جنبه في نفس الفولدر.
3. Board: **ESP32 Dev Module**، والـ Port: الـ COM بتاع الـ ESP.
4. دوس **Upload**. لما يخلص هيكتب `Hash of data verified`.
5. **افصل كابل الـ USB** وشغّل البوردة على الـ 12V.

> **ملحوظة عن Arduino IDE:** البرنامج ساعات بيفضل فاكر نسخة قديمة من الملف وهو مفتوح. لو عدّلت الملفات من بره البرنامج، افتح المشروع من فولدر جديد باسم جديد.

> لو عدّلت `app.html` أو `diag.html`، لازم تعمل ملفات `.h` تاني:
> ```bash
> python3 -c "h=open('app.html',encoding='utf-8').read();open('app_html.h','w',encoding='utf-8').write('#pragma once\n#include <pgmspace.h>\nconst char APP_HTML[] PROGMEM = R\"APPHTML('+h+')APPHTML\";\n')"
> python3 -c "h=open('diag.html',encoding='utf-8').read();open('diag_html.h','w',encoding='utf-8').write('#pragma once\n#include <pgmspace.h>\nconst char DIAG_HTML[] PROGMEM = R\"DIAG('+h+')DIAG\";\n')"
> ```

## بيعمل إيه

- **شبكة واي فاي (SoftAP):** اسمها `NeuralNexus-Screen` وباسوردها `12345678`، على قناة 6، والعنوان `192.168.4.1`. وفيها DNS بيوجّه الموبايل للصفحة لوحده (Captive portal)، وmDNS بالاسم `screen.local`.
- **الشاشة:** بيدوّر لوحده على الأطراف والسرعة الصح ويحفظها. ولو الشاشة مابتردش، بيكمل يبعت على **TX0 وTX17 مع بعض** (وضع "blind").
- **الإعداد اللي بيكتبه في الخانة (SP 0x3000):** ترميز GB2312، Font0=24، Font1=28، حجم 40×40، لون أسود.
- **الرسايل:** يحفظ لحد 30 رسالة، ويثبّت رسالة، ويعرض رسالة لعدد دقايق محدد، ويجدول لحد 20 رسالة بالساعة (الساعة بتتظبط من الموبايل). وكله بيتحفظ في الذاكرة الداخلية (Preferences).
- **لما مفيش رسالة:** بيعرض "BORG EL ARAB TECHNOLOGICAL UNIVERSITY".

## الـ API

| الطلب | الوظيفة |
|---|---|
| `GET /api/status` | الحالة: النسخة، وسبب آخر تشغيل، والشاشة، والواي فاي، والرسالة الحالية |
| `POST /api/send` `text, dur, save` | يعرض رسالة (`dur` بالثواني، و0 يعني دايمًا) |
| `POST /api/clear` | يرجّع الكلام الافتراضي |
| `GET/POST /api/messages/*` | add / update / delete / pin / show |
| `GET/POST /api/schedules/*` | add / delete |
| `POST /api/time` `epoch` | يظبط الساعة |
| `POST /api/screen/test` | يتأكد إن الشاشة بترد |
| `POST /api/screen/detect` | يدوّر على الأطراف والسرعة من جديد |
| `POST /api/screen/pins` `rx,tx,baud,inv,crc` | إعداد يدوي |
| `POST /api/diag/scan` | يجرب 56 إعداد ويرجّع كل البايتات اللي وصلت |
| `POST /api/diag/write` `path=tx0/tx17, inv, text` | يكتب على طريق واحد بس |
| `POST /api/diag/font` `f0,f1,x,y,enc,color,text` | يجرب خط وحجم وترميز ولون على طول |
| `GET /api/log` | آخر الأحداث |
| `POST /api/wifi` `ssid, pass` | يغيّر اسم وباسورد الشبكة (ولو الإعداد غلط بيرجع للأصلي) |

## أوامر السيريال (Serial Monitor على 115200، والشاشة مش على RX0)

`status`، `say <text>`، `clear`، `detect`، `ping`، `log`، `sniff`، `cfg <rx> <tx> <baud> <inv> <crc>`، `raw <hex>`
