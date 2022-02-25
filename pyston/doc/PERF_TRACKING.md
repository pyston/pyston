## Server setup

```
git clone http://github.com/kmod/codespeed --branch pyston
virtualenv codespeed_env -p pyston
source codespeed_env/bin/activate
cd codespeed
pip install -r requirements
python manage.py migrate
python manage.py createsuperuser
python manage.py collectstatic

cp sample_project/deploy/apache-reverseproxy.conf /etc/apache2/sites-available/010-speed.pyston.conf
ln -s /etc/apache2/sites-available/010-speed.pyston.conf /etc/apache2/sites-enabled
# may need:
# a2enmod proxy_http
# service apache2 restart
service apache2 reload

sudo snap install core; sudo snap refresh core
sudo snap install --classic certbot
sudo ln -s /snap/bin/certbot /usr/bin/certbot
sudo certbot --apache
```

To run:
```
~/codesped_env/bin/python codespeed/manage.py runserver 8000
```

Go to the admin site and an "environment", a "project", and an "executable".
Create an "environment" for each computer that it will be run on and a new "project".
