一、所需环境
python 3.7
mysql 5.7以上
二、使用说明
1、安装python mysql
2、pip install requirements.txt
3、修改SURI_OPEN\settings.py数据库信息
4、python manage.py migrate生成django框架得迁移信息表
5、在数据库中导入mysql数据表
6、前端页面使用免费框架AdminLTE
7、python manage.py runserver 127.0.0.1:8099启动界面看到页面即成功
8、若需要uwsgi启动，修改script\uwsgi.ini

三、监控使用说明
suricata\suri_monitor.py启动监控任务，可配置成定时任务
配置文件请修改suricata\config.py文件，mysql数据库，邮件服务器，ELASTIC地址，以及suricata和pafirewall的索引名称
配合elastic存储的suricata日志，可以监控suricata状态
suricata信息也可联动防火墙日志信息，更精准定位


