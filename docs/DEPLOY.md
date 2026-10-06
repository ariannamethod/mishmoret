# הפעלה ותחזוקה

בתחילת הדרך השרת פועל אצל אולג; בהמשך מעבירים אותו ל־Raspberry Pi שבמרכז. ארבעת חברי הצוות עובדים על אותו קוד ב־GitHub, ולכל אחד חשבון משלו באפליקציה.

## הגדרות השרת

אפשר להריץ את `scripts/manage.sh` מכל תיקייה. ברירת המחדל היא שמירת הנתונים ב־`data/` בתוך תיקיית הפרויקט. בהתקנה הקבועה משתמשים בתיקייה נפרדת, שבבעלות המשתמש שמפעיל את השירות:

```sh
export MISHMERET_DATA_DIR=/var/lib/mishmeret
export MISHMERET_PORT=8080
export MISHMERET_ORIGIN=https://YOUR-HOST.YOUR-TAILNET.ts.net
./scripts/manage.sh start
```

מחליפים את כתובת הדוגמה בכתובת האמיתית. `MISHMERET_ORIGIN` היא הכתובת המדויקת שפותחים בדפדפן, ללא `/` בסוף. אחרי שינוי כתובת מפעילים מחדש. בהפעלה מקומית הכתובת היא `http://127.0.0.1:8080`. השרת מקבל חיבורים ב־`127.0.0.1`; Tailscale מעביר אליו את הגישה מבחוץ דרך HTTPS.

## קישור למשתתפים

Tailscale מותקן במחשב השרת. כדי שהמשתתפים יוכלו להיכנס בקישור רגיל, מפעילים עליו Funnel:

```sh
tailscale funnel --bg 8080
tailscale funnel status
```

מעתיקים את כתובת ה־HTTPS מהפלט אל `MISHMERET_ORIGIN` ומפעילים מחדש את האפליקציה. המשתתפים פותחים את הקישור ונכנסים לחשבון שלהם; אין צורך להתקין אצלם Tailscale. לכיבוי הגישה הזאת:

```sh
tailscale funnel --https=443 off
```

לניסוי בתוך רשת Tailscale סגורה אפשר להשתמש ב־`tailscale serve --bg 8080` במקום Funnel. באפשרות הזאת גם המכשירים של המשתתפים צריכים להיות מחוברים ל־Tailscale. הפעלת השיתוף היא צעד ידני של צוות התחזוקה. [מדריך Funnel](https://tailscale.com/docs/reference/tailscale-cli/funnel) · [מדריך Serve](https://tailscale.com/docs/reference/tailscale-cli/serve).

## גיבוי ושחזור

מריצים עם אותו `MISHMERET_DATA_DIR` שמשמש את השרת:

```sh
./scripts/manage.sh backup
```

הפקודה יוצרת צילום עקבי של מסד הנתונים גם בזמן שהאפליקציה פועלת, ומדפיסה את מיקום הקובץ בתיקיית `backups/` שבתוך תיקיית הנתונים. שומרים עותק גם במחשב נוסף.

לפני שחזור מגבים את המצב הנוכחי ועוצרים את השרת עם `Ctrl-C` או `sudo systemctl stop mishmeret`. אחר כך:

```sh
./scripts/manage.sh restore /absolute/path/to/backup.db
./scripts/manage.sh start
```

מחליפים את הנתיב בנתיב הגיבוי שרוצים לשחזר. הפקודה בודקת את הקובץ ומשחזרת רק כשהשרת עצור. הנתונים מוחלפים בנתוני הגיבוי, וכל המשתמשים מתבקשים להיכנס מחדש עם הסיסמאות שהיו בזמן הגיבוי.

## מעבר ל־Raspberry Pi

1. מורידים את הקוד ומתקינים את התלויות לפי README, ואז מריצים `make` על ה־Pi.
2. עוצרים את השרת הישן ומריצים `./scripts/manage.sh backup`.
3. מעבירים את קובץ הגיבוי ל־Pi דרך `scp`. תחת המשתמש שמפעיל את השירות יוצרים תיקיית נתונים בהרשאות `700` ושומרים בה את הקובץ בשם `mishmeret.db`, בהרשאות `600`.
4. מגדירים את כתובת ה־HTTPS החדשה ומפעילים את השרת. החשבונות וההרשמות מגיעים עם מסד הנתונים. בודקים כניסה, הצגת הלוח ושמירת הרשמה; השרת הישן נשאר עצור.

להפעלה אוטומטית עם הדלקת ה־Pi יש [קובץ systemd לדוגמה](../scripts/mishmeret.service.example). הוא משתמש בקוד שבתיקיית `/opt/mishmeret`, במשתמש שירות בשם `mishmeret` ובנתונים שב־`/var/lib/mishmeret`. יוצרים את המשתמש והתיקיות, נותנים לו בעלות על תיקיית הנתונים, ומגדירים ב־`/etc/mishmeret.env`:

```ini
MISHMERET_DATA_DIR=/var/lib/mishmeret
MISHMERET_PORT=8080
MISHMERET_ORIGIN=https://YOUR-HOST.YOUR-TAILNET.ts.net
```

מעתיקים את קובץ הדוגמה אל `/etc/systemd/system/mishmeret.service`, ואז:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now mishmeret
sudo journalctl -u mishmeret -n 100 --no-pager
```

הפקודה האחרונה מציגה את יומן השירות, כדי לבדוק מה קרה בהפעלה.

## תחזוקה משותפת ועדכונים

לכל אחד מהארבעה גישה ל־GitHub, חשבון SSH ומפתח אישי ב־Pi. רשת Tailscale של המרכז מאפשרת לצוות התחזוקה להגיע ל־SSH. הרשאות ״דיקטטור״ משמשות לניהול לוח הלימודים; הרשאות השרת מאפשרות לעדכן קוד, להפעיל את השירות ולגבות נתונים. מגדירים לצוות הרשאות לתיקיית הפרויקט ולפעולות השירות האלה.

בשלב הראשון אולג מאשר וממזג שינויים; בהמשך אפשר להרחיב את הרשאות המיזוג לצוות. כדי להתקין שינוי שאושר, רושמים את הגרסה שפועלת עכשיו בעזרת `git rev-parse HEAD`, יוצרים גיבוי ועוצרים את השירות. מחליפים את `COMMIT_SHA` במזהה הקומיט שנבחר מתוך `main`:

```sh
git fetch origin main
git switch --detach COMMIT_SHA
make
make check
sudo systemctl start mishmeret
```

חזרה לגרסה קודמת: עוצרים את השירות, עוברים למזהה שנשמר ובונים מחדש. אם מבנה מסד הנתונים השתנה בין הגרסאות, משחזרים גם את הגיבוי המתאים. העדכון בשרת נעשה ביוזמת צוות התחזוקה; מיזוג ב־GitHub שומר את הקוד החדש במאגר.
