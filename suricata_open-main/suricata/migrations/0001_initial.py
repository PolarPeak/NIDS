# Generated by Django 2.0.7 on 2021-11-10 10:47

from django.db import migrations, models


class Migration(migrations.Migration):

    initial = True

    dependencies = [
    ]

    operations = [
        migrations.CreateModel(
            name='SuriMonitor',
            fields=[
                ('id', models.AutoField(auto_created=True, primary_key=True, serialize=False, verbose_name='ID')),
                ('project', models.CharField(max_length=63)),
                ('sid', models.IntegerField(blank=True, null=True)),
                ('times', models.IntegerField(blank=True, null=True)),
                ('cnts', models.IntegerField(blank=True, null=True)),
                ('threatname', models.CharField(blank=True, max_length=255, null=True)),
                ('tips', models.CharField(blank=True, max_length=255, null=True)),
            ],
            options={
                'db_table': 'suri_monitor',
                'managed': False,
            },
        ),
    ]
