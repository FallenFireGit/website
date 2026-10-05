const navLinks = document.querySelectorAll('header a');

navLinks.forEach(link => {
    link.addEventListener('click', function(e) {
        e.preventDefault();

        const targetId = this.getAttribute('href');
        const targetSection = document.querySelector(targetId);

        targetSection.scrollIntoView({ behavior: 'smooth' });
    });
});

const sections = document.querySelectorAll('section');

window.addEventListener('scroll', function() {
    let currentSection = '';

    sections.forEach(section => {
        const sectionTop = section.offsetTop - 80;

        if (window.scrollY >= sectionTop) {
            currentSection = section.getAttribute('id');
        }
    });

    navLinks.forEach(link => {
        link.classList.remove('active');

        if (link.getAttribute('href') === '#' + currentSection) {
            link.classList.add('active');
        }
    });
});

const faders = document.querySelectorAll('.fade-in');

const observer = new IntersectionObserver(function(entries) {
    entries.forEach(entry => {
        if (entry.isIntersecting) {
            entry.target.classList.add('visible');
        }
    });
}, { threshold: 0.1 });


faders.forEach(fader => {
    observer.observe(fader);
});

const githubUsername = 'FallenFireGit';

fetch(`https://api.github.com/users/${githubUsername}/repos?sort=updated&per_page=6`)
    .then(response => {
        if (!response.ok) {
            throw new Error('GitHub API returned ' + response.status);
        }
        return response.json();
    })
    .then(repos => {
        const container = document.getElementById('github-projects');
        container.innerHTML = '';

        repos.forEach(repo => {
            const card = document.createElement('div');
            card.classList.add('github-card');

            // textContent instead of innerHTML so repo text can't inject HTML
            const name = document.createElement('h3');
            name.textContent = repo.name;

            const description = document.createElement('p');
            description.textContent = repo.description || 'No description provided.';

            const link = document.createElement('a');
            link.href = repo.html_url;
            link.target = '_blank';
            link.rel = 'noopener';
            link.textContent = 'View on Github';

            card.append(name, description, link);
            container.appendChild(card);
        });
    })
    .catch(error => {
        document.getElementById('github-projects').innerHTML = '<p>Could not load projects.</p>';
        console.error(error);
    });