from django.db import models
from django.db.models.fields import TextField


class suri_log(models.Model):
    id = models.AutoField(primary_key=True)
    time = models.DateField()
    iface = models.CharField(max_length=12)
    attack_ip = models.CharField(max_length=50)
    victim_ip = models.CharField(max_length=50)
    level = models.CharField(max_length=20)


